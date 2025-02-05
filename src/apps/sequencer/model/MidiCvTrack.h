#pragma once

#include "Config.h"
#include "Types.h"
#include "MidiConfig.h"
#include "ModelUtils.h"
#include "Serialize.h"
#include "Arpeggiator.h"

#include "core/math/Math.h"

class MidiCvTrack {
public:
    //----------------------------------------
    // Types
    //----------------------------------------

    enum class VoiceConfig {
        Pitch,
        PitchVelocity,
        PitchVelocityPressure,
        Last,
    };

    static const char *voiceConfigName(VoiceConfig voiceConfig) {
        switch (voiceConfig) {
        case VoiceConfig::Pitch:                return "Pitch";
        case VoiceConfig::PitchVelocity:        return "Pitch+Vel";
        case VoiceConfig::PitchVelocityPressure:return "Pitch+Vel+Press";
        case VoiceConfig::Last:                 break;
        }
        return nullptr;
    }

    enum class NotePriority : uint8_t {
        LastNote,
        FirstNote,
        LowestNote,
        HighestNote,
        Last,
    };

    static const char *notePriorityName(NotePriority notePriority) {
        switch (notePriority) {
        case NotePriority::LastNote:    return "Last Note";
        case NotePriority::FirstNote:   return "First Note";
        case NotePriority::LowestNote:  return "Lowest Note";
        case NotePriority::HighestNote: return "Highest Note";
        case NotePriority::Last:        break;
        }
        return nullptr;
    }

    //----------------------------------------
    // Properties
    //----------------------------------------

    // source

    const MidiSourceConfig &source() const { return _source; }
          MidiSourceConfig &source()       { return _source; }

    // voices

    int voices() const { return _voices; }
    void setVoices(int voices) {
        _voices = clamp(voices, 1, 8);
    }

    void editVoices(int value, bool shift) {
        setVoices(voices() + value);
    }

    void printVoices(StringBuilder &str) const {
        str("%d", voices());
    }

    // voiceConfig

    VoiceConfig voiceConfig() const { return _voiceConfig; }
    void setVoiceConfig(VoiceConfig voiceConfig) {
        _voiceConfig = ModelUtils::clampedEnum(voiceConfig);
    }

    void editVoiceConfig(int value, bool shift) {
        setVoiceConfig(ModelUtils::adjustedEnum(voiceConfig(), value));
    }

    void printVoiceConfig(StringBuilder &str) const {
        str(voiceConfigName(_voiceConfig));
    }

    // notePriority

    NotePriority notePriority() const { return _notePriority; }
    void setNotePriority(NotePriority notePriority) {
        _notePriority = notePriority;
    }

    void editNotePriority(int value, bool shift) {
        setNotePriority(ModelUtils::adjustedEnum(notePriority(), value));
    }

    void printNotePriority(StringBuilder &str) const {
        str(notePriorityName(notePriority()));
    }

    // lowNote

    int lowNote() const { return _lowNote; }
    void setLowNote(int lowNote) {
        _lowNote = clamp(lowNote, 0, highNote());
    }

    void editLowNote(int value, bool shift) {
        setLowNote(lowNote() + value * (shift ? 12 : 1));
    }

    void printLowNote(StringBuilder &str) const {
        Types::printMidiNote(str, lowNote());
    }

    // highNote

    int highNote() const { return _highNote; }
    void setHighNote(int highNote) {
        _highNote = clamp(highNote, lowNote(), 127);
    }

    void editHighNote(int value, bool shift) {
        setHighNote(highNote() + value * (shift ? 12 : 1));
    }

    void printHighNote(StringBuilder &str) const {
        Types::printMidiNote(str, highNote());
    }

    // pitchBendRange

    int pitchBendRange() const { return _pitchBendRange; }
    void setPitchBendRange(int pitchBendRange) {
        _pitchBendRange = clamp(pitchBendRange, 0, 48);
    }

    void editPitchBendRange(int value, bool shift) {
        setPitchBendRange(pitchBendRange() + value);
    }

    void printPitchBendRange(StringBuilder &str) const {
        if (_pitchBendRange == 0) {
            str("off");
        } else {
            str("%d semitones", _pitchBendRange);
        }
    }

    // modulationRange

    Types::VoltageRange modulationRange() const { return _modulationRange; }
    void setModulationRange(Types::VoltageRange modulationRange) {
        _modulationRange = ModelUtils::clampedEnum(modulationRange);
    }

    void editModulationRange(int value, bool shift) {
        setModulationRange(ModelUtils::adjustedEnum(modulationRange(), value));
    }

    void printModulationRange(StringBuilder &str) const {
        str(Types::voltageRangeName(modulationRange()));
    }

    // retrigger

    bool retrigger() const { return _retrigger; }
    void setRetrigger(bool retrigger) {
        _retrigger = retrigger;
    }

    void editRetrigger(int value, bool shift) {
        setRetrigger(value > 0);
    }

    void printRetrigger(StringBuilder &str) const {
        ModelUtils::printYesNo(str, retrigger());
    }

    // arpeggiator

    const Arpeggiator &arpeggiator() const { return _arpeggiator; }
          Arpeggiator &arpeggiator()       { return _arpeggiator; }

    //----------------------------------------
    // Methods
    //----------------------------------------

    MidiCvTrack() { clear(); }

    void clear();

    void gateOutputName(int index, StringBuilder &str) const;
    void cvOutputName(int index, StringBuilder &str) const;

    void write(WriteContext &context) const;
    void read(ReadContext &context);

private:
    void setTrackIndex(int trackIndex) {
        _trackIndex = trackIndex;
    }

    int8_t _trackIndex = -1;
    MidiSourceConfig _source;
    uint8_t _voices;
    VoiceConfig _voiceConfig;
    NotePriority _notePriority;
    uint8_t _lowNote;
    uint8_t _highNote;
    uint8_t _pitchBendRange;
    Types::VoltageRange _modulationRange;
    bool _retrigger;
    Arpeggiator _arpeggiator;

    friend class Track;
};
