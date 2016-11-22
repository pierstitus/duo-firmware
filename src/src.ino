/*
  DATO DUO firmware
  (c) 2016, David Menting <david@dato.mu>

  Hardware:
  - Runs on a Teensy 3.1/3.2 or on the DUO brains with Kinetis K20DX256
  - Output goes to the K20's 12 bit DAC output
  - A button matrix is connected to the pins specified in pinmap.h
  - LED drivers connected as specified in Leds.h
  
  The main loop() defines the sequencer behaviour. Parameters are updated
  in read_pots() and keys are handled in handle_keys().

*/
#include "Arduino.h"
#include <Keypad.h>
#include "midinotes.h"
#include <MIDI.h>

//#define NO_POTS
//#define NO_AUDIO
#include "pinmap.h"
#include "Buttons.h"

const int MIDI_CHANNEL = 1;
int gate_length_msec = 40;
const int SYNC_LENGTH_MSEC = 1;
const int MIN_TEMPO_MSEC = 666; // Tempo is actually an interval in ms

// Sequencer settings
const int NUM_STEPS = 8;

unsigned char current_step = 0;
unsigned char target_step = 0;
unsigned int tempo = 0;
unsigned long next_step_time = 0;
unsigned long gate_off_time = 0;
unsigned long sync_off_time = 0;
unsigned long note_on_time;
unsigned long previous_note_on_time;
unsigned long note_off_time;
// Define the array of leds

char set_key = 9;

// Musical settings
//const int BLACK_KEYS[] = {22,25,27,30,32,34,37,39,42,44,46,49,51,54,56,58,61,63,66,68,73,75,78,80};
const int SCALE[] = { 49,51,54,56,58,61,63,66,68,70 }; // Low with 2 note split
const float SAMPLERATE_STEPS[] = { 44100,4435,2489,1109 }; 
const char DETUNE_OFFSET_SEMITONES[] = { 3,4,5,7,9 };
#define INITIAL_VELOCITY 100

// Global variables
int detune_amount = 0;
int osc1_frequency = 0;
uint8_t osc1_midi_note = 0;
bool note_is_playing = 0;
bool note_is_triggered = false;
bool note_is_played = false;
bool double_speed = false;
int transpose = 0;
bool next_step_is_random = false;
int num_notes_held = 0;
int tempo_interval;
bool random_flag = 0;
bool power_flag = 1;

uint16_t keyboard_map = 0;
uint16_t old_keyboard_map = 0;
const uint16_t KEYBOARD_MASK = 0b11111111111;

void read_pots();
void midi_note_on(uint8_t channel, uint8_t note, uint8_t velocity);
void midi_note_off(uint8_t channel, uint8_t note, uint8_t velocity);
void note_on(uint8_t midi_note, uint8_t velocity, bool enabled);
void note_off();
float midi_note_to_frequency(int x) ;
int detune(int note, int amount);
void handle_input_until(unsigned long until);
void handle_keys();
void handle_midi();
int tempo_interval_msec();
void midi_init();
void power_off();
void power_on();
void keyboard_to_note();

MIDI_CREATE_DEFAULT_INSTANCE();

#include "Synth.h"
#include "Sequencer.h"
#include "Leds.h"

void setup() {

  Serial.begin(57600);
  audio_init();

  midi_init();

  led_init();

  keys_init();

  pins_init();

  Serial.println("Dato DUO firmware");
  previous_note_on_time = millis();

  #ifdef NO_AUDIO
  digitalWrite(AMP_ENABLE, LOW);
  #else
  digitalWrite(AMP_ENABLE, HIGH);
  #endif

  sequencer_stop();
}

void loop() {

  if(power_flag != 0) {
    handle_keys();

    if(sequencer_is_running) {
      sequencer();
    } else {
      keyboard_to_note();          
    }
    handle_midi();
    read_pots();
    update_leds();
  } else {
    pinMode(row_pins[1],INPUT_PULLUP);
    pinMode(col_pins[2],OUTPUT);
    digitalWrite(col_pins[2],LOW);
    //TODO: Why do I need to force these LEDs low?
    analogWrite(ENV_LED, 0);
    analogWrite(FILTER_LED, 0);
    analogWrite(OSC_LED, 0);
    // Low power delay
    delay(100);
    //TODO: has to have been low at least once
    if(!digitalRead(row_pins[1])) {
      power_on();
      sequencer_start();
    }
    pinMode(col_pins[2],INPUT_PULLUP);
    delay(20);
  }
}

void keyboard_to_note() {
  // If the old map was zero and now it's not, turn on the right note
  if((old_keyboard_map & KEYBOARD_MASK) != (keyboard_map & KEYBOARD_MASK)) {
    // We're starting from the top. High notes have priority
    for(int i = 10; i >= 0; i--) {
      if(bitRead(keyboard_map,i)){
        note_on(SCALE[i]+transpose, INITIAL_VELOCITY, true);
        break;
      }
    }
  }

  if((old_keyboard_map & KEYBOARD_MASK) && (keyboard_map & KEYBOARD_MASK)==0) {
    // If the old map was not zero and now it is, turn off the note
    note_off();
  }
  old_keyboard_map = keyboard_map;

}
// Scans the keypad and handles step enable and keys
void handle_keys() {
  if (keypad.getKeys())  {
    for (int i=0; i<LIST_MAX; i++) {
      if ( keypad.key[i].stateChanged ) {
        char k = keypad.key[i].kchar;
        switch (keypad.key[i].kstate) {  // Report active key state : IDLE, PRESSED, HOLD, or RELEASED
            case PRESSED:   
                if (k <= KEYB_9 && k >= KEYB_0) {
                  bitSet(keyboard_map,(k - KEYB_0));
                  if(sequencer_is_running) {
                    step_note[target_step] = k - KEYB_0;
                    step_enable[target_step] = 1;
                    step_velocity[target_step] = INITIAL_VELOCITY; 
                  } else {
                    current_step++;
                    if (current_step >= NUM_STEPS) current_step = 0;
                    target_step=current_step;
                    step_note[target_step] = k - KEYB_0;
                    step_enable[target_step] = 1;
                    step_velocity[target_step] = INITIAL_VELOCITY; 
                  }
                } else if (k <= STEP_8 && k >= STEP_1) {
                  step_enable[k-STEP_1] = 1-step_enable[k-STEP_1];
                  if(!step_enable[k-STEP_1]) { leds(k-STEP_1) = CRGB::Black; }
                  step_velocity[k-STEP_1] = INITIAL_VELOCITY;
                } else if (k == BTN_SEQ2) {
                  double_speed = true;
                } else if (k == BTN_DOWN) {
                  transpose--;
                  if(transpose<-12){transpose = -24;}
                } else if (k == BTN_UP) {
                  transpose++;
                  if(transpose>12){transpose = 24;}
                } else if (k == BTN_SEQ1) {
                  next_step_is_random = true;
                  random_flag = true;
                } else if (k == SEQ_START) {
                  if(sequencer_is_running) {
                    sequencer_stop();
                  } else {
                    power_on();
                    sequencer_start();
                  }
                }
                break;
            case HOLD:
                if (k == SEQ_START) {
                  power_off();
                }
                break;
            case RELEASED:
                if (k <= KEYB_9 && k >= KEYB_0) {
                  MIDI.sendNoteOff(SCALE[k-KEYB_0]+transpose,64,MIDI_CHANNEL);
                  bitClear(keyboard_map,(k - KEYB_0));
                } else if (k == BTN_SEQ2) {
                  double_speed = false;
                } else if (k == BTN_DOWN) {
                  if(transpose<-12){transpose = -12;}
                  if(transpose>12){transpose = 12;}
                } else if (k == BTN_UP) {
                  if(transpose<-12){transpose = -12;}
                  if(transpose>12){transpose = 12;}
                } else if (k == BTN_SEQ1) {
                  next_step_is_random = false;
                } 
                break;
            case IDLE:
                break;
        }
      }
    }
  } 
}

void handle_midi() {
  MIDI.read();
}

void read_pots() {

  // If the DUO brains are running without pots attached, do nothing
  #ifdef NO_POTS
    return;
  #endif

  // Read out the pots/switches
  gate_length_msec = map(analogRead(GATE_POT),1023,0,10,200);
  int volume_pot_value = analogRead(FADE_POT);
  int resonance = analogRead(FILTER_RES_POT);
  int amp_env_release = map(analogRead(AMP_ENV_POT),0,1023,30,300);
  int filter_pot_value = analogRead(FILTER_FREQ_POT);
  detune_amount = 1023-analogRead(OSC_DETUNE_POT);
  int pulse_pot_value = analogRead(OSC_PW_POT);

  analogWrite(FILTER_LED, filter_pot_value>>2);
  analogWrite(OSC_LED, 255-(pulse_pot_value>>2));

  // Audio interrupts have to be off to apply settings
  AudioNoInterrupts();

  // TODO: mix away the oscillator at the end of the range
  waveform2.frequency(detune(osc1_midi_note,detune_amount));
  waveform2.pulseWidth(map(pulse_pot_value,0,1023,1000,50)/1000.0);

  filter1.frequency(map(filter_pot_value,0,1023,60,300));
  //filter1.frequency(fscale(0.,1023.,60.,300.,filter_pot_value,0));

  // TODO: do exponential filter pot behaviour
  filter1.resonance(map(resonance,0,1023,500,70)/100.0); // 0.7-5.0 range

  envelope1.release(amp_env_release);

  if(digitalRead(BITC_PIN)) {
    bitcrusher1.sampleRate(SAMPLERATE_STEPS[0]);
  } else {
    bitcrusher1.sampleRate(SAMPLERATE_STEPS[2]);
  }
  
  if(digitalRead(NOISE_PIN)) {
    noise1.amplitude(0.0);
  } else {
    noise1.amplitude(0.3);
  }

  mixer2.gain(0, map(volume_pot_value,0,1023,1000,10)/1000.);
  mixer2.gain(1, map(volume_pot_value,0,1023,700,70)/1000.);

  AudioInterrupts(); 
}

void midi_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
  note_on(note, velocity, true);
}

void midi_note_off(uint8_t channel, uint8_t note, uint8_t velocity) {
  note_off();
}

void note_on(uint8_t midi_note, uint8_t velocity, bool enabled) {

  // Override velocity if button on the synth is pressed
  if(!digitalRead(BTN_SYN1)) {
    velocity = 127;
  }

  note_is_playing = midi_note;

  if(enabled) {
    AudioNoInterrupts();

    dc1.amplitude(velocity / 127.); // DC amplitude controls filter env amount.
    osc1_midi_note = midi_note;
    osc1_frequency = (int)midi_note_to_frequency(midi_note);
    waveform1.frequency(osc1_frequency);
    // Detune OSC2
    waveform2.frequency(detune(osc1_midi_note,detune_amount));

    AudioInterrupts(); 

    MIDI.sendNoteOn(midi_note, velocity, MIDI_CHANNEL);
    envelope1.noteOn();
    envelope2.noteOn();
  } else {
    // Set LED to white but don't play a note
    leds(current_step) = LED_WHITE;
  }
}

void note_off() {
  if (note_is_playing) {
    if(!step_enable[current_step]) {
      leds(current_step) = CRGB::Black;
    } else {
      envelope1.noteOff();
      envelope2.noteOff();
      MIDI.sendNoteOff(note_is_playing, 64, MIDI_CHANNEL);
    }
    note_is_playing = 0;
  } 
}

float midi_note_to_frequency(int x) {
  return MIDI_NOTE_FREQUENCY[x];
}

int detune(int note, int amount) { // amount goes from 0-1023
  if (amount > 800) {
    return midi_note_to_frequency(note) * (amount+9000)/10000.;
  } else if (amount < 100) {
    return midi_note_to_frequency(note - 12) * ( 20000 - amount )/20000.;
  } else {
    int offset = map(amount,200,900,4,0);
    return midi_note_to_frequency(note - DETUNE_OFFSET_SEMITONES[offset]);
  }
}

int tempo_interval_msec() {
  #ifdef NO_POTS
    return 300;
  #endif
  int potvalue = analogRead(TEMPO_POT);
  return map(potvalue,10,1023,40,MIN_TEMPO_MSEC);
  
}

void midi_init() {
  MIDI.begin(MIDI_CHANNEL);
  MIDI.setHandleNoteOn(midi_note_on);
  MIDI.setHandleNoteOff(midi_note_off);
}

void power_off() { // TODO: this is super crude and doesn't work, but it shows the effect
  sequencer_stop();
  AudioNoInterrupts();
  digitalWrite(AMP_ENABLE, LOW);

  for(int i = 32; i >= 0; i--) {
    FastLED.setBrightness(i);
    FastLED.show();
    analogWrite(ENV_LED,i);
    analogWrite(FILTER_LED,i);
    analogWrite(OSC_LED,i);
    delay(20);
  }
  FastLED.clear();
  FastLED.show();
  power_flag = 0;
  pinMode(row_pins[1],INPUT_PULLUP);
  pinMode(col_pins[2],OUTPUT);
  digitalWrite(col_pins[2],LOW);
  while(!digitalRead(row_pins[1])) {
    // wait for release of the power button
  }
  delay(100);
}

void power_on() {
  led_init();
  AudioInterrupts();
  digitalWrite(AMP_ENABLE, HIGH);
  power_flag = 1;
}