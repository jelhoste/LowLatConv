// LowLatConv : plugin DPF (CLAP, VST3, LV2...) autour du moteur de convolution à latence nulle.
#include "DistrhoPlugin.hpp"

#include "processor.hpp"
#include "stereo_engine.hpp"
#include "wav_loader.hpp"

#include "extra/Mutex.hpp"      // DPF's own portable Thread / Mutex / sleep: std::thread and std::mutex are not
#include "extra/Sleep.hpp"      // available with every MinGW (Windows cross-compilation) thread model
#include "extra/Thread.hpp"

#include <atomic>
#include <string>

START_NAMESPACE_DISTRHO

#include "LowLatConvParams.h"
#include "LowLatConvIR.hpp"
using namespace LLCParams;

// ---------------------------------------------------------------------------------------------

class LowLatConvPlugin : public Plugin
{
public:
    LowLatConvPlugin() : Plugin(pCount, 0, 1)
    {
        for (uint32_t i = 0; i < pCount; ++i) vals_[i].store(kDefs[i].def, std::memory_order_relaxed);
        sr_.store(getSampleRate());
        proc_.prepare(getSampleRate());
        pushParams(true);
        worker_.start();
    }
    ~LowLatConvPlugin() override
    {
        worker_.stopThread(3000);
    }

protected:
    const char* getLabel() const override { return "LowLatConv"; }
    const char* getDescription() const override { return "Zero-latency convolution (cabinet / impulse response loader) with EQ."; }
    const char* getMaker() const override { return "LowLatConv authors"; }
    const char* getHomePage() const override { return ""; }
    const char* getLicense() const override { return "Open source (licence to be chosen)"; }
    uint32_t getVersion() const override { return d_version(0, 1, 0); }
    int64_t getUniqueId() const override { return d_cconst('L', 'L', 'C', 'v'); }

    void initParameter(uint32_t index, Parameter& p) override
    {
        const ParamDef& d = kDefs[index];
        p.hints = 0;
        if (d.flags & kFlagAuto) p.hints |= kParameterIsAutomatable;
        if (d.flags & kFlagLog)  p.hints |= kParameterIsLogarithmic;
        if (d.flags & kFlagBool) p.hints |= kParameterIsBoolean;
        if (d.flags & kFlagTrigger) p.hints |= kParameterIsTrigger;
        if (d.flags & kFlagInt)  p.hints |= kParameterIsInteger;
        if (d.flags & kFlagOut)  p.hints |= kParameterIsOutput;
        if (d.flags & kFlagHidden) p.hints |= kParameterIsHidden;
        p.name = d.name; p.shortName = d.name; p.symbol = d.symbol; p.unit = d.unit;
        p.ranges.def = d.def; p.ranges.min = d.min; p.ranges.max = d.max;
        if (d.nLabels > 0)
        {
            p.enumValues.count = uint8_t(d.nLabels);
            p.enumValues.restrictedMode = true;
            p.enumValues.values = new ParameterEnumerationValue[size_t(d.nLabels)];
            for (int i = 0; i < d.nLabels; ++i) p.enumValues.values[i] = ParameterEnumerationValue(float(i), d.labels[i]);
        }
    }

    void initState(uint32_t, State& s) override
    {
        s.hints = kStateIsFilenamePath;
        s.key = kStateIRPath;
        s.label = "Impulse response";
        s.defaultValue = "";
    }

    float getParameterValue(uint32_t index) const override { return vals_[index].load(std::memory_order_relaxed); }

    void setParameterValue(uint32_t index, float value) override
    {
        if (index >= pCount || kDefs[index].flags & kFlagOut) return;
        value = std::min(kDefs[index].max, std::max(kDefs[index].min, value == value ? value : kDefs[index].def));
        vals_[index].store(value, std::memory_order_relaxed);
        if (index == pMeterReset) { if (value > 0.5f) resetPeak_.store(true, std::memory_order_release); return; }
        if (index >= kIRFirst && index <= kIRLast) irStamp_.fetch_add(1, std::memory_order_relaxed);
        else paramsDirty_.store(true, std::memory_order_release);
    }

    void setState(const char* key, const char* value) override
    {
        if (std::strcmp(key, kStateIRPath) != 0) return;
        { const MutexLocker lk(pathMutex_); irPath_ = value ? value : ""; }
        irStamp_.fetch_add(1, std::memory_order_relaxed);
    }

    String getState(const char* key) const override
    {
        if (std::strcmp(key, kStateIRPath) != 0) return String();
        const MutexLocker lk(pathMutex_);
        return String(irPath_.c_str());
    }

    void sampleRateChanged(double sr) override
    {
        sr_.store(sr);
        proc_.prepare(sr);
        pushParams(true);
        irStamp_.fetch_add(1, std::memory_order_relaxed);        // the IR must be re-prepared for the new rate
    }

    void activate() override { proc_.reset(); }

    void run(const float** in, float** out, uint32_t frames) override
    {
        if (paramsDirty_.exchange(false, std::memory_order_acquire)) pushParams(false);
        if (resetPeak_.exchange(false, std::memory_order_acq_rel)) { proc_.resetMeterPeakMax(); vals_[pMeterReset].store(0.0f, std::memory_order_relaxed); }   // DPF does not rearm triggers itself
        proc_.process(in[0], in[1], out[0], out[1], int(frames));
        publishMeters();
    }

private:
    float v(Param p) const { return vals_[p].load(std::memory_order_relaxed); }

    void pushParams(bool snap)
    {
        llc::ProcessorParams q;
        q.inputGainDb = v(pInGain); q.outputGainDb = v(pOutGain); q.dryDb = v(pDry); q.wetDb = v(pWet);
        q.preDelayMs = v(pPreDelay); q.width = v(pWidth) / 100.0; q.eqEnabled = v(pEqOn) > 0.5f;
        static const Param bandBase[3] = {pB1On, pB2On, pB3On};
        for (int b = 0; b < 3; ++b) {
            const uint32_t o = bandBase[b];
            q.eq.band[b].enabled = vals_[o].load() > 0.5f;
            q.eq.band[b].type = llc::BandType(int(vals_[o + 1].load() + 0.5f));
            q.eq.band[b].freq = vals_[o + 2].load(); q.eq.band[b].gainDb = vals_[o + 3].load(); q.eq.band[b].q = vals_[o + 4].load();
        }
        q.eq.hp = {v(pHpOn) > 0.5f, v(pHpFreq), 12 << int(v(pHpSlope) + 0.5f)};
        q.eq.lp = {v(pLpOn) > 0.5f, v(pLpFreq), 12 << int(v(pLpSlope) + 0.5f)};
        proc_.setParams(q, snap);
        if (v(pMeterRmsMs) != lastRmsMs_) { lastRmsMs_ = v(pMeterRmsMs); proc_.setMeterRmsMs(lastRmsMs_); }
        if (v(pMeterPeakDecay) != lastDecay_) { lastDecay_ = v(pMeterPeakDecay); proc_.setMeterPeakDecay(lastDecay_); }
    }

    void publishMeters()
    {
        const llc::LevelMeters::Frame f = proc_.takeMeterFrame();
        for (int i = 0; i < 4; ++i) {
            vals_[pMInPeakL + uint32_t(i)].store(f.peak[i], std::memory_order_relaxed);
            vals_[pMInPeakMaxL + uint32_t(i)].store(f.peakMax[i], std::memory_order_relaxed);
            vals_[pMInRmsL + uint32_t(i)].store(f.rms[i], std::memory_order_relaxed);
        }
        vals_[pMInLufsM].store(f.lufsM[0], std::memory_order_relaxed); vals_[pMOutLufsM].store(f.lufsM[1], std::memory_order_relaxed);
        vals_[pMInLufsS].store(f.lufsS[0], std::memory_order_relaxed); vals_[pMOutLufsS].store(f.lufsS[1], std::memory_order_relaxed);
    }

    // Thread de chargement : débounce, lecture du WAV, préparation de l'IR, construction du moteur.
    // Si une nouvelle demande arrive pendant un chargement, le résultat périmé est abandonné (jamais
    // installé) et le thread repart avec la demande la plus récente. En cas d'erreur l'IR est déchargée
    // (le plugin repasse en bypass) pour rester cohérent avec le chemin sauvegardé.
    void workerLoop()
    {
        uint32_t built = 0;
        llc::IRBuffer raw; std::string rawPath;
        while (!worker_.shouldThreadExit())
        {
            d_msleep(20);
            proc_.collectRetired();
            const uint32_t stamp = irStamp_.load();
            if (stamp == built) continue;
            d_msleep(60);                                                    // débounce des réglages
            if (irStamp_.load() != stamp) continue;
            built = stamp;

            std::string path; { const MutexLocker lk(pathMutex_); path = irPath_; }
            if (path.empty()) { rawPath.clear(); proc_.unloadIR(); status(0, 0); continue; }
            status(1, 0);
            if (path != rawPath) {
                const bool ok = llc::loadWavFile(path, raw) == llc::WavStatus::Ok;
                if (irStamp_.load() != stamp) continue;                      // périmé : une demande plus récente attend
                if (!ok) { rawPath.clear(); proc_.unloadIR(); status(3, 0); continue; }
                rawPath = path;
            }
            float values[pCount];
            for (uint32_t i = 0; i < pCount; ++i) values[i] = vals_[i].load(std::memory_order_relaxed);
            const llc::IROptions o = makeIROptions(values, sr_.load());
            llc::IRBuffer ir;
            if (!llc::processIR(raw, o, ir)) { if (irStamp_.load() == stamp) { proc_.unloadIR(); status(3, 0); } continue; }
            auto engine = llc::StereoEngine::build(ir, makeInputMode(values));
            if (irStamp_.load() != stamp) continue;                          // périmé : on ne l'installe pas
            if (!engine) { proc_.unloadIR(); status(3, 0); continue; }
            const float ms = float(1000.0 * double(ir.length()) / ir.sampleRate);
            proc_.loadEngine(std::move(engine));
            status(2, ms);
        }
    }
    void status(int s, float ms) { vals_[pIRStatus].store(float(s)); vals_[pIRLengthMs].store(ms); }

    std::atomic<float> vals_[pCount];
    std::atomic<bool> paramsDirty_{false};
    std::atomic<uint32_t> irStamp_{0};
    std::atomic<double> sr_{48000.0};
    float lastRmsMs_ = -1.0f, lastDecay_ = -1.0f;
    std::atomic<bool> resetPeak_{false};
    mutable Mutex pathMutex_;
    std::string irPath_;
    llc::ConvolutionProcessor proc_;
    struct Worker : Thread {
        explicit Worker(LowLatConvPlugin* p) : Thread("LowLatConvLoader"), plugin(p) {}
        void start() { startThread(); }
        void run() override { plugin->workerLoop(); }
        LowLatConvPlugin* const plugin;
    } worker_{this};

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LowLatConvPlugin)
};

Plugin* createPlugin() { return new LowLatConvPlugin(); }

END_NAMESPACE_DISTRHO
