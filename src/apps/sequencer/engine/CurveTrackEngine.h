#pragma once

#include "TrackEngine.h"
#include "SequenceState.h"
#include "SortedQueue.h"
#include "CurveRecorder.h"

#include "model/Track.h"

class CurveTrackEngine : public TrackEngine {
public:
    CurveTrackEngine(Engine &engine, const Model &model, Track &track, const TrackEngine *linkedTrackEngine) :
        TrackEngine(engine, model, track, linkedTrackEngine),
        _curveTrack(track.curveTrack())
    {
        reset();
    }

    virtual Track::TrackMode trackMode() const override { return Track::TrackMode::Curve; }

    virtual void reset() override;
    virtual void restart() override;
    virtual void tick(uint32_t tick) override;
    virtual void update(float dt) override;

    virtual void changePattern() override;

    virtual const TrackLinkData *linkData() const override { return &_linkData; }

    virtual bool activity() const override { return _activity; }
    virtual bool gateOutput(int index) const override { return _gateOutput; }
    virtual float cvOutput(int index) const override { return _cvOutput; }
    virtual float sequenceProgress() const override {
        return _currentStep < 0 ? 0.f : float(_currentStep - _sequence->firstStep()) / (_sequence->lastStep() - _sequence->firstStep());
    }

    const CurveSequence &sequence() const { return *_sequence; }
    bool isActiveSequence(const CurveSequence &sequence) const { return &sequence == _sequence; }

    int currentStep() const { return _currentStep; }
    float currentStepFraction() const { return _currentStepFraction; }

private:
    void triggerStep(uint32_t tick, uint32_t divisor);
    void updateOutput(uint32_t relativeTick, uint32_t divisor);

    bool isRecording() const;
    void updateRecordValue();
    void updateRecording(uint32_t relativeTick, uint32_t divisor);

    uint32_t applySwing(uint32_t tick) const;

    CurveTrack &_curveTrack;

    TrackLinkData _linkData;

    float _recordValue;
    CurveRecorder _recorder;

    CurveSequence *_sequence;
    CurveSequence *_fillSequence;
    SequenceState _sequenceState;
    int _currentStep;
    float _currentStepFraction;
    bool _shapeVariation;
    CurveTrack::FillMode _fillMode;

    bool _activity;
    bool _gateOutput;
    float _cvOutput = 0.f;
    float _cvOutputTarget = 0.f;

    struct Gate {
        uint32_t tick;
        bool gate;
    };

    struct GateCompare {
        bool operator()(const Gate &a, const Gate &b) {
            return a.tick < b.tick;;
        }
    };

    SortedQueue<Gate, 16, GateCompare> _gateQueue;
};
