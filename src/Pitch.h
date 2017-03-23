#ifndef Pitch_h
#define Pitch_h
/*
  handles oscillator 1 pitch, oscillator 2 pitch
  also does pitch bend and glide

  oscillator 1 is the master pitch. It's a pulse wave with variable pulse width
  Oscillator 2 can be detuned in steps
*/

const uint8_t DETUNE_OFFSET_SEMITONES[] = { 3,4,5,7,9 };
int detune_amount = 0;
uint32_t pitch_update_time = 0;
void pitch_update();

// Constant rate glide
void pitch_update() {
  detune_amount = muxAnalogRead(OSC_DETUNE_POT);
  float osc_saw_target_frequency = detune(osc_pulse_midi_note,detune_amount);

  const float GLIDE_COEFFICIENT = 0.3f;
  if(!muxDigitalRead(SLIDE_PIN)) {
    if(pitch_update_time < millis()) {
      osc_saw_frequency = osc_saw_frequency + (osc_saw_target_frequency - osc_saw_frequency)*GLIDE_COEFFICIENT;
      osc_pulse_frequency = osc_pulse_frequency + (osc_pulse_target_frequency - osc_pulse_frequency)*GLIDE_COEFFICIENT;
      pitch_update_time = millis() + 10;
    }
  } else {
    osc_saw_frequency = osc_saw_target_frequency;
    osc_pulse_frequency = osc_pulse_target_frequency;
  }
}

float detune(int note, int amount) { // amount goes from 0-1023
  if (amount > 800) {
    return midi_note_to_frequency(note) * (amount+9000)/10000.;
  } else if (amount < 200) {
    return midi_note_to_frequency(note - 12) * ( 20000 - amount )/20000.;
  } else {
    int offset = map(amount,200,800,4,0);
    return midi_note_to_frequency(note - DETUNE_OFFSET_SEMITONES[offset]);
  }
}

#endif