#include "CurveTrackEngine.h"

#include "Engine.h"
#include "Groove.h"
#include "SequenceUtils.h"

#include "core/Debug.h"
#include "core/utils/Random.h"
#include "core/math/Math.h"

#include "model/Curve.h"
#include "model/Types.h"

static Random rng;

static float evalStepShape(const CurveSequence::Step &step, bool variation, bool invert, float fraction) {
    auto function = Curve::function(Curve::Type(variation ? step.shapeVariation() : step.shape()));
    float value = function(fraction);
    if (invert) {
        value = 1.f - value;
    }
    float min = float(step.min()) / CurveSequence::Min::Max;
    float max = float(step.max()) / CurveSequence::Max::Max;
    return min + value * (max - min);
}

static bool evalShapeVariation(const CurveSequence::Step &step, int probabilityBias) {
    int probability = clamp(step.shapeVariationProbability() + probabilityBias, 0, 8);
    return int(rng.nextRange(8)) < probability;
}

static bool evalGate(const CurveSequence::Step &step, int probabilityBias) {
    int probability = clamp(step.gateProbability() + probabilityBias, -1, CurveSequence::GateProbability::Max);
    return int(rng.nextRange(CurveSequence::GateProbability::Range)) <= probability;
}

void CurveTrackEngine::reset() {
    _sequenceState.reset();
    _currentStep = -1;
    _currentStepFraction = 0.f;
    _shapeVariation = false;
    _fillMode = CurveTrack::FillMode::None;
    _activity = false;
    _gateOutput = false;

    _recorder.reset();
    _gateQueue.clear();

    changePattern();
}

void CurveTrackEngine::restart() {
    _sequenceState.reset();
    _currentStep = -1;
    _currentStepFraction = 0.f;
}

void CurveTrackEngine::tick(uint32_t tick) {
    ASSERT(_sequence != nullptr, "invalid sequence");
    const auto &sequence = *_sequence;
    const auto *linkData = _linkedTrackEngine ? _linkedTrackEngine->linkData() : nullptr;

    if (linkData) {
        _linkData = *linkData;
        _sequenceState = *linkData->sequenceState;

        updateRecording(linkData->relativeTick, linkData->divisor);

        if (linkData->relativeTick % linkData->divisor == 0) {
            triggerStep(tick, linkData->divisor);
        }

        updateOutput(linkData->relativeTick, linkData->divisor);
    } else {
        uint32_t divisor = sequence.divisor() * (CONFIG_PPQN / CONFIG_SEQUENCE_PPQN);
        uint32_t resetDivisor = sequence.resetMeasure() * _engine.measureDivisor();
        uint32_t relativeTick = resetDivisor == 0 ? tick : tick % resetDivisor;

        // handle reset measure
        if (relativeTick == 0) {
            reset();
        }

        updateRecording(relativeTick, divisor);

        if (relativeTick % divisor == 0) {
            // advance sequence
            switch (_curveTrack.playMode()) {
            case Types::PlayMode::Aligned:
                _sequenceState.advanceAligned(relativeTick / divisor, sequence.runMode(), sequence.firstStep(), sequence.lastStep(), rng);
                triggerStep(tick, divisor);
                break;
            case Types::PlayMode::Free:
                _sequenceState.advanceFree(sequence.runMode(), sequence.firstStep(), sequence.lastStep(), rng);
                triggerStep(tick, divisor);
                break;
            case Types::PlayMode::Last:
                break;
            }
        }

        updateOutput(relativeTick, divisor);

        _linkData.divisor = divisor;
        _linkData.relativeTick = relativeTick;
        _linkData.sequenceState = &_sequenceState;
    }

    while (!_gateQueue.empty() && tick >= _gateQueue.front().tick) {
        _activity = _gateQueue.front().gate;
        _gateOutput = (!mute() || fill()) && _activity;
        _gateQueue.pop();

        _engine.midiOutputEngine().sendGate(_track.trackIndex(), _gateOutput);
    }
}

void CurveTrackEngine::update(float dt) {
    // override due to recording
    if (isRecording()) {
        updateRecordValue();
        const auto &range = Types::voltageRangeInfo(_sequence->range());
        _cvOutputTarget = range.denormalize(_recordValue);
        _cvOutput = _cvOutputTarget;
    }

    if (!mute()) {
        if (_curveTrack.slideTime() > 0) {
            float factor = 1.f - 0.01f * _curveTrack.slideTime();
            factor = 500.f * factor * factor;
            _cvOutput += (_cvOutputTarget - _cvOutput) * std::min(1.f, dt * factor);
        } else {
            _cvOutput = _cvOutputTarget;
        }
    }
}

void CurveTrackEngine::changePattern() {
    _sequence = &_curveTrack.sequence(pattern());
    _fillSequence = &_curveTrack.sequence(std::min(pattern() + 1, CONFIG_PATTERN_COUNT - 1));
}

void CurveTrackEngine::triggerStep(uint32_t tick, uint32_t divisor) {
    int rotate = _curveTrack.rotate();
    int shapeProbabilityBias = _curveTrack.shapeProbabilityBias();
    int gateProbabilityBias = _curveTrack.gateProbabilityBias();

    const auto &sequence = *_sequence;
    _currentStep = SequenceUtils::rotateStep(_sequenceState.step(), sequence.firstStep(), sequence.lastStep(), rotate);
    const auto &step = sequence.step(_currentStep);

    _shapeVariation = evalShapeVariation(step, shapeProbabilityBias);

    bool fillStep = fill() && (rng.nextRange(100) < uint32_t(fillAmount()));
    _fillMode = fillStep ? _curveTrack.fillMode() : CurveTrack::FillMode::None;

    // Trigger gate pattern
    int gate = step.gate();
    for (int i = 0; i < 4; ++i) {
        if (gate & (1 << i) && evalGate(step, gateProbabilityBias)) {
            uint32_t gateStart = (divisor * i) / 4;
            uint32_t gateLength = divisor / 8;
            _gateQueue.pushReplace({ applySwing(tick + gateStart), true });
            _gateQueue.pushReplace({ applySwing(tick + gateStart + gateLength), false });
        }
    }
}

void CurveTrackEngine::updateOutput(uint32_t relativeTick, uint32_t divisor) {
    if (_sequenceState.step() < 0) {
        return;
    }

    bool fillVariation = _fillMode == CurveTrack::FillMode::Variation;
    bool fillNextPattern = _fillMode == CurveTrack::FillMode::NextPattern;
    bool fillInvert = _fillMode == CurveTrack::FillMode::Invert;

    const auto &sequence = *_sequence;
    const auto &range = Types::voltageRangeInfo(sequence.range());

    const auto &evalSequence = fillNextPattern ? *_fillSequence : *_sequence;
    const auto &step = evalSequence.step(_currentStep);

    _currentStepFraction = float(relativeTick % divisor) / divisor;

    float value = evalStepShape(step, _shapeVariation || fillVariation, fillInvert, _currentStepFraction);
    value = range.denormalize(value);
    _cvOutputTarget = value;

    _engine.midiOutputEngine().sendCv(_track.trackIndex(), _cvOutputTarget);
}

bool CurveTrackEngine::isRecording() const {
    return
        _engine.state().recording() &&
        _model.project().curveCvInput() != Types::CurveCvInput::Off &&
        _model.project().selectedTrackIndex() == _track.trackIndex();
}

void CurveTrackEngine::updateRecordValue() {
    auto &sequence = *_sequence;
    const auto &range = Types::voltageRangeInfo(sequence.range());
    auto curveCvInput = _model.project().curveCvInput();

    switch (curveCvInput) {
    case Types::CurveCvInput::Cv1:
    case Types::CurveCvInput::Cv2:
    case Types::CurveCvInput::Cv3:
    case Types::CurveCvInput::Cv4:
        _recordValue = range.normalize(_engine.cvInput().channel(int(curveCvInput) - int(Types::CurveCvInput::Cv1)));
        break;
    default:
        _recordValue = 0.f;
        break;
    }
}

void CurveTrackEngine::updateRecording(uint32_t relativeTick, uint32_t divisor) {
    if (!isRecording()) {
        _recorder.reset();
        return;
    }

    updateRecordValue();

    if (_recorder.write(relativeTick, divisor, _recordValue) && _sequenceState.step() >= 0) {
        auto &sequence = *_sequence;
        int rotate = _curveTrack.rotate();
        auto &step = sequence.step(SequenceUtils::rotateStep(_sequenceState.step(), sequence.firstStep(), sequence.lastStep(), rotate));
        auto match = _recorder.matchCurve();
        step.setShape(match.type);
        step.setMinNormalized(match.min);
        step.setMaxNormalized(match.max);
    }
}

uint32_t CurveTrackEngine::applySwing(uint32_t tick) const {
    return Groove::swing(tick, CONFIG_PPQN / 4, swing());
}
