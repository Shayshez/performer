#include "NoteTrackEngine.h"

#include "Engine.h"
#include "Groove.h"
#include "SequenceUtils.h"

#include "core/Debug.h"
#include "core/utils/Random.h"
#include "core/math/Math.h"

#include "model/Scale.h"

static Random rng;

// evaluate if step gate is active
static bool evalStepGate(const NoteSequence::Step &step, int probabilityBias) {
    int probability = clamp(step.gateProbability() + probabilityBias, -1, NoteSequence::GateProbability::Max);
    return step.gate() && int(rng.nextRange(NoteSequence::GateProbability::Range)) <= probability;
}

// evaluate step condition
static bool evalStepCondition(const NoteSequence::Step &step, int iteration, bool fill, bool &prevCondition) {
    auto condition = step.condition();
    switch (condition) {
    case Types::Condition::Off:                                         return true;
    case Types::Condition::Fill:        prevCondition = fill;           return prevCondition;
    case Types::Condition::NotFill:     prevCondition = !fill;          return prevCondition;
    case Types::Condition::Pre:                                         return prevCondition;
    case Types::Condition::NotPre:                                      return !prevCondition;
    case Types::Condition::First:       prevCondition = iteration == 0; return prevCondition;
    case Types::Condition::NotFirst:    prevCondition = iteration != 0; return prevCondition;
    default:
        int index = int(condition);
        if (index >= int(Types::Condition::Loop) && index < int(Types::Condition::Last)) {
            auto loop = Types::conditionLoop(condition);
            prevCondition = iteration % loop.base == loop.offset;
            return prevCondition;
        }
    }
    return true;
}

// evaluate step retrigger count
static int evalStepRetrigger(const NoteSequence::Step &step, int probabilityBias) {
    int probability = clamp(step.retriggerProbability() + probabilityBias, -1, NoteSequence::RetriggerProbability::Max);
    return int(rng.nextRange(NoteSequence::RetriggerProbability::Range)) <= probability ? step.retrigger() + 1 : 1;
}

// evaluate step length
static int evalStepLength(const NoteSequence::Step &step, int lengthBias) {
    int length = NoteSequence::Length::clamp(step.length() + lengthBias) + 1;
    int probability = step.lengthVariationProbability();
    if (int(rng.nextRange(NoteSequence::LengthVariationProbability::Range)) <= probability) {
        int offset = step.lengthVariationRange() == 0 ? 0 : rng.nextRange(std::abs(step.lengthVariationRange()) + 1);
        if (step.lengthVariationRange() < 0) {
            offset = -offset;
        }
        length = clamp(length + offset, 0, NoteSequence::Length::Range);
    }
    return length;
}

// evaluate transposition
static int evalTransposition(const Scale &scale, int octave, int transpose) {
    return octave * scale.notesPerOctave() + transpose;
}

// evaluate note voltage
static float evalStepNote(const NoteSequence::Step &step, int probabilityBias, const Scale &scale, int rootNote, int octave, int transpose, bool useVariation = true) {
    int note = step.note() + (scale.isChromatic() ? rootNote : 0) + evalTransposition(scale, octave, transpose);
    int probability = clamp(step.noteVariationProbability() + probabilityBias, -1, NoteSequence::NoteVariationProbability::Max);
    if (useVariation && int(rng.nextRange(NoteSequence::NoteVariationProbability::Range)) <= probability) {
        int offset = step.noteVariationRange() == 0 ? 0 : rng.nextRange(std::abs(step.noteVariationRange()) + 1);
        if (step.noteVariationRange() < 0) {
            offset = -offset;
        }
        note = NoteSequence::Note::clamp(note + offset);
    }
    return scale.noteToVolts(note);
}

void NoteTrackEngine::reset() {
    _freeRelativeTick = 0;
    _sequenceState.reset();
    _currentStep = -1;
    _prevCondition = false;
    _activity = false;
    _gateOutput = false;
    _cvOutput = 0.f;
    _cvOutputTarget = 0.f;
    _slideActive = false;
    _gateQueue.clear();
    _cvQueue.clear();
    _recordHistory.clear();

    changePattern();
}

void NoteTrackEngine::restart() {
    _freeRelativeTick = 0;
    _sequenceState.reset();
    _currentStep = -1;
}

void NoteTrackEngine::tick(uint32_t tick) {
    ASSERT(_sequence != nullptr, "invalid sequence");
    const auto &sequence = *_sequence;
    const auto *linkData = _linkedTrackEngine ? _linkedTrackEngine->linkData() : nullptr;

    if (linkData) {
        _linkData = *linkData;
        _sequenceState = *linkData->sequenceState;

        if (linkData->relativeTick % linkData->divisor == 0) {
            recordStep(tick, linkData->divisor);
            triggerStep(tick, linkData->divisor);
        }
    } else {
        uint32_t divisor = sequence.divisor() * (CONFIG_PPQN / CONFIG_SEQUENCE_PPQN);
        uint32_t resetDivisor = sequence.resetMeasure() * _engine.measureDivisor();
        uint32_t relativeTick = resetDivisor == 0 ? tick : tick % resetDivisor;

        // handle reset measure
        if (relativeTick == 0) {
            reset();
        }

        // advance sequence
        switch (_noteTrack.playMode()) {
        case Types::PlayMode::Aligned:
            if (relativeTick % divisor == 0) {
                _sequenceState.advanceAligned(relativeTick / divisor, sequence.runMode(), sequence.firstStep(), sequence.lastStep(), rng);
                recordStep(tick, divisor);
                triggerStep(tick, divisor);
            }
            break;
        case Types::PlayMode::Free:
            relativeTick = _freeRelativeTick;
            if (++_freeRelativeTick >= divisor) {
                _freeRelativeTick = 0;
            }
            if (relativeTick == 0) {
                _sequenceState.advanceFree(sequence.runMode(), sequence.firstStep(), sequence.lastStep(), rng);
                recordStep(tick, divisor);
                triggerStep(tick, divisor);
            }
            break;
        case Types::PlayMode::Last:
            break;
        }

        _linkData.divisor = divisor;
        _linkData.relativeTick = relativeTick;
        _linkData.sequenceState = &_sequenceState;
    }

    auto &midiOutputEngine = _engine.midiOutputEngine();

    while (!_gateQueue.empty() && tick >= _gateQueue.front().tick) {
        _activity = _gateQueue.front().gate;
        _gateOutput = (!mute() || fill()) && _activity;
        _gateQueue.pop();

        midiOutputEngine.sendGate(_track.trackIndex(), _gateOutput);
    }

    while (!_cvQueue.empty() && tick >= _cvQueue.front().tick) {
        if (!mute() || _noteTrack.cvUpdateMode() == NoteTrack::CvUpdateMode::Always) {
            _cvOutputTarget = _cvQueue.front().cv;
            _slideActive = _cvQueue.front().slide;

            midiOutputEngine.sendCv(_track.trackIndex(), _cvOutputTarget);
            midiOutputEngine.sendSlide(_track.trackIndex(), _slideActive);
        }
        _cvQueue.pop();
    }
}

void NoteTrackEngine::update(float dt) {
    bool running = _engine.state().running();
    bool recording = _engine.state().recording();

    const auto &sequence = *_sequence;
    const auto &scale = sequence.selectedScale(_model.project().scale());
    int rootNote = sequence.selectedRootNote(_model.project().rootNote());
    int octave = _noteTrack.octave();
    int transpose = _noteTrack.transpose();

    // enable/disable step recording mode
    if (_engine.recording() && _model.project().recordMode() == Types::RecordMode::StepRecord) {
        if (_currentRecordStep == -1) {
            _currentRecordStep = _sequence->firstStep();
        }
    } else {
        _currentRecordStep = -1;
    }

    // override due to monitoring or recording
    bool isStepRecordMode = _model.project().recordMode() == Types::RecordMode::StepRecord;
    if (!running && (!recording || isStepRecordMode) && _monitorStepIndex >= 0) {
        // step monitoring (first priority)
        const auto &step = sequence.step(_monitorStepIndex);
        _cvOutputTarget = evalStepNote(step, 0, scale, rootNote, octave, transpose, false);
        _activity = _gateOutput = true;
        _monitorOverrideActive = true;
    } else if ((!running || !isStepRecordMode) && _recordHistory.isNoteActive()) {
        // midi monitoring (second priority)
        int note = noteFromMidiNote(_recordHistory.activeNote()) + evalTransposition(scale, octave, transpose);
        _cvOutputTarget = scale.noteToVolts(note);
        _activity = _gateOutput = true;
        _monitorOverrideActive = true;
    } else {
        if (_monitorOverrideActive) {
            _activity = _gateOutput = false;
            _monitorOverrideActive = false;
        }
    }

    if (_slideActive && _noteTrack.slideTime() > 0) {
        _cvOutput += (_cvOutputTarget - _cvOutput) * std::min(1.f, dt * (200 - 2 * _noteTrack.slideTime()));
    } else {
        _cvOutput = _cvOutputTarget;
    }
}

void NoteTrackEngine::changePattern() {
    _sequence = &_noteTrack.sequence(pattern());
    _fillSequence = &_noteTrack.sequence(std::min(pattern() + 1, CONFIG_PATTERN_COUNT - 1));
}

void NoteTrackEngine::monitorMidi(uint32_t tick, const MidiMessage &message) {
    _recordHistory.write(tick, message);

    if (_engine.recording() && _model.project().recordMode() == Types::RecordMode::StepRecord) {
        if (message.isNoteOn()) {
            // record to step
            auto &step = _sequence->step(_currentRecordStep);
            step.setGate(true);
            step.setNote(noteFromMidiNote(message.note()));

            // move to next step
            ++_currentRecordStep;
            if (_currentRecordStep > _sequence->lastStep()) {
                _currentRecordStep = _sequence->firstStep();
            }
        }
    }
}

void NoteTrackEngine::setMonitorStep(int index) {
    _monitorStepIndex = (index >= 0 && index < CONFIG_STEP_COUNT) ? index : -1;

    // in step record mode, select step to start recording recording from
    if (_engine.recording() && _model.project().recordMode() == Types::RecordMode::StepRecord &&
        index >= _sequence->firstStep() && index <= _sequence->lastStep()) {
        _currentRecordStep = index;
    }
}

void NoteTrackEngine::triggerStep(uint32_t tick, uint32_t divisor) {
    int octave = _noteTrack.octave();
    int transpose = _noteTrack.transpose();
    int rotate = _noteTrack.rotate();
    bool fillStep = fill() && (rng.nextRange(100) < uint32_t(fillAmount()));
    bool useFillGates = fillStep && _noteTrack.fillMode() == NoteTrack::FillMode::Gates;
    bool useFillSequence = fillStep && _noteTrack.fillMode() == NoteTrack::FillMode::NextPattern;
    bool useFillCondition = fillStep && _noteTrack.fillMode() == NoteTrack::FillMode::Condition;

    const auto &sequence = *_sequence;
    const auto &evalSequence = useFillSequence ? *_fillSequence : *_sequence;
    _currentStep = SequenceUtils::rotateStep(_sequenceState.step(), sequence.firstStep(), sequence.lastStep(), rotate);
    const auto &step = evalSequence.step(_currentStep);

    uint32_t gateOffset = (divisor * step.gateOffset()) / (NoteSequence::GateOffset::Max + 1);

    bool stepGate = evalStepGate(step, _noteTrack.gateProbabilityBias()) || useFillGates;
    if (stepGate) {
        stepGate = evalStepCondition(step, _sequenceState.iteration(), useFillCondition, _prevCondition);
    }

    if (stepGate) {
        uint32_t stepLength = (divisor * evalStepLength(step, _noteTrack.lengthBias())) / NoteSequence::Length::Range;
        int stepRetrigger = evalStepRetrigger(step, _noteTrack.retriggerProbabilityBias());
        if (stepRetrigger > 1) {
            uint32_t retriggerLength = divisor / stepRetrigger;
            uint32_t retriggerOffset = 0;
            while (stepRetrigger-- > 0 && retriggerOffset <= stepLength) {
                _gateQueue.pushReplace({ applySwing(tick + gateOffset + retriggerOffset), true });
                _gateQueue.pushReplace({ applySwing(tick + gateOffset + retriggerOffset + retriggerLength / 2), false });
                retriggerOffset += retriggerLength;
            }
        } else {
            _gateQueue.pushReplace({ applySwing(tick + gateOffset), true });
            _gateQueue.pushReplace({ applySwing(tick + gateOffset + stepLength), false });
        }
    }

    if (stepGate || _noteTrack.cvUpdateMode() == NoteTrack::CvUpdateMode::Always) {
        const auto &scale = evalSequence.selectedScale(_model.project().scale());
        int rootNote = evalSequence.selectedRootNote(_model.project().rootNote());
        _cvQueue.push({ applySwing(tick + gateOffset), evalStepNote(step, _noteTrack.noteProbabilityBias(), scale, rootNote, octave, transpose), step.slide() });
    }
}

void NoteTrackEngine::recordStep(uint32_t tick, uint32_t divisor) {
    if (!_engine.state().recording() || _model.project().recordMode() == Types::RecordMode::StepRecord || _sequenceState.prevStep() < 0) {
        return;
    }

    bool stepWritten = false;

    auto writeStep = [this, divisor, &stepWritten] (int stepIndex, int note, int lengthTicks) {
        auto &step = _sequence->step(stepIndex);
        int length = (lengthTicks * NoteSequence::Length::Range) / divisor;

        step.setGate(true);
        step.setGateProbability(NoteSequence::GateProbability::Max);
        step.setRetrigger(0);
        step.setRetriggerProbability(NoteSequence::RetriggerProbability::Max);
        step.setLength(length);
        step.setLengthVariationRange(0);
        step.setLengthVariationProbability(NoteSequence::LengthVariationProbability::Max);
        step.setNote(noteFromMidiNote(note));
        step.setNoteVariationRange(0);
        step.setNoteVariationProbability(NoteSequence::NoteVariationProbability::Max);
        step.setCondition(Types::Condition::Off);

        stepWritten = true;
    };

    auto clearStep = [this] (int stepIndex) {
        auto &step = _sequence->step(stepIndex);

        step.clear();
    };

    uint32_t stepStart = tick - divisor;
    uint32_t stepEnd = tick;
    uint32_t margin = divisor / 2;

    for (size_t i = 0; i < _recordHistory.size(); ++i) {
        if (_recordHistory[i].type != RecordHistory::Type::NoteOn) {
            continue;
        }

        int note = _recordHistory[i].note;
        uint32_t noteStart = _recordHistory[i].tick;
        uint32_t noteEnd = i + 1 < _recordHistory.size() ? _recordHistory[i + 1].tick : tick;

        if (noteStart >= stepStart - margin && noteStart < stepStart + margin) {
            // note on during step start phase
            if (noteEnd >= stepEnd) {
                // note hold during step
                int length = std::min(noteEnd, stepEnd) - stepStart;
                writeStep(_sequenceState.prevStep(), note, length);
            } else {
                // note released during step
                int length = noteEnd - noteStart;
                writeStep(_sequenceState.prevStep(), note, length);
            }
        } else if (noteStart < stepStart && noteEnd > stepStart) {
            // note on during previous step
            int length = std::min(noteEnd, stepEnd) - stepStart;
            writeStep(_sequenceState.prevStep(), note, length);
        }
    }

    if (isSelected() && !stepWritten && _model.project().recordMode() == Types::RecordMode::Overwrite) {
        clearStep(_sequenceState.prevStep());
    }
}

uint32_t NoteTrackEngine::applySwing(uint32_t tick) const {
    return Groove::swing(tick, CONFIG_PPQN / 4, swing());
}

int NoteTrackEngine::noteFromMidiNote(uint8_t midiNote) const {
    const auto &scale = _sequence->selectedScale(_model.project().scale());
    int rootNote = _sequence->selectedRootNote(_model.project().rootNote());

    if (scale.isChromatic()) {
        return scale.noteFromVolts((midiNote - 60 - rootNote) * (1.f / 12.f));
    } else {
        return scale.noteFromVolts((midiNote - 60) * (1.f / 12.f));
    }
}
