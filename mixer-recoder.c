#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#ifdef _WIN32
  #include <direct.h>
  #include <conio.h>
  #define MKDIR(dir) _mkdir(dir)
  #define READ_KEY() _getch()
#else
  #include <sys/stat.h>
  #include <termios.h>
  #include <unistd.h>
  #define MKDIR(dir) mkdir(dir, 0755)

  void set_terminal_raw(int enable) {
      static struct termios oldt, newt;
      if (enable) {
          tcgetattr(STDIN_FILENO, &oldt);
          newt = oldt;
          newt.c_lflag &= ~(ICANON | ECHO);
          tcsetattr(STDIN_FILENO, TCSANOW, &newt);
      } else {
          tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
      }
  }

  int READ_KEY(void) {
      return getchar();
  }
#endif

#define MAX_TRACKS 8
#define SAMPLE_RATE 48000
#define PI 3.14159265358979323846f

typedef struct {
    ma_encoder encoders[MAX_TRACKS];
    ma_decoder decoders[MAX_TRACKS];
    ma_bool32   isTrackInitialized[MAX_TRACKS];
    ma_bool32   hasRecordedData[MAX_TRACKS];
    char        trackNames[MAX_TRACKS][256];
    char        trackPaths[MAX_TRACKS][512];
    float       trackVolumes[MAX_TRACKS];
    int         selectedTrack;
    int         activeTrackIndex;
    char        sessionFolder[256];
    
    // Metronome Variables
    ma_bool32   metronomeOn;
    float       bpm;
    ma_uint64   sampleCounter;
    ma_uint64   samplesPerBeat;
    int         clickSampleLength;
    
    char        statusMessage[256];
} MultitrackSystem;

float generate_metronome_sample(MultitrackSystem* pSystem) {
    if (!pSystem->metronomeOn) return 0.0f;
    ma_uint64 positionInBeat = pSystem->sampleCounter % pSystem->samplesPerBeat;
    if (positionInBeat < (ma_uint64)pSystem->clickSampleLength) {
        float frequency = (pSystem->sampleCounter % (pSystem->samplesPerBeat * 4) < pSystem->samplesPerBeat) ? 1000.0f : 800.0f;
        float t = (float)positionInBeat / SAMPLE_RATE;
        float sample = sinf(2.0f * PI * frequency * t);
        float decay = 1.0f - ((float)positionInBeat / pSystem->clickSampleLength);
        return sample * decay * 0.4f;
    }
    return 0.0f;
}

void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    MultitrackSystem* pSystem = (MultitrackSystem*)pDevice->pUserData;
    if (pSystem == NULL) return;

    int currentTrack = pSystem->activeTrackIndex;

    // 1. CAPTURE INPUT
    if (pInput != NULL && currentTrack >= 0 && currentTrack < MAX_TRACKS && pSystem->isTrackInitialized[currentTrack]) {
        ma_encoder_write_pcm_frames(&pSystem->encoders[currentTrack], pInput, frameCount, NULL);
        pSystem->hasRecordedData[currentTrack] = MA_TRUE; 
    }

    // 2. PLAYBACK & OVERDUB
    if (pOutput != NULL) {
        float* pOutFloat = (float*)pOutput;
        for (ma_uint32 i = 0; i < frameCount * 2; i++) pOutFloat[i] = 0.0f;
        float trackBuffer[512 * 2]; 

        ma_uint32 framesProcessed = 0;
        while (framesProcessed < frameCount) {
            ma_uint32 chunkFrames = frameCount - framesProcessed;
            if (chunkFrames > 512) chunkFrames = 512;

            for (int t = 0; t < MAX_TRACKS; t++) {
                if (t != currentTrack && pSystem->hasRecordedData[t]) {
                    ma_uint64 framesRead = 0;
                    ma_decoder_read_pcm_frames(&pSystem->decoders[t], trackBuffer, chunkFrames, &framesRead);
                    
                    float volume = pSystem->trackVolumes[t];
                    for (ma_uint32 i = 0; i < framesRead * 2; i++) {
                        pOutFloat[(framesProcessed * 2) + i] += trackBuffer[i] * volume;
                    }
                    if (framesRead < chunkFrames) {
                        ma_decoder_seek_to_pcm_frame(&pSystem->decoders[t], 0);
                    }
                }
            }

            for (ma_uint32 f = 0; f < chunkFrames; f++) {
                float click = generate_metronome_sample(pSystem);
                pSystem->sampleCounter++;
                pOutFloat[(framesProcessed * 2) + (f * 2)]     += click;
                pOutFloat[(framesProcessed * 2) + (f * 2) + 1] += click;
            }
            framesProcessed += chunkFrames;
        }
    }
}

void render_ui(MultitrackSystem* pSystem) {
    // Clear screen cleanly on keypress
    #ifdef _WIN32
        system("cls");
    #else
        printf("\033[2J\033[H");
    #endif

    printf("SIMPLE RECODER\n\n");
    printf(" Session Directory: ./%-45s\n", pSystem->sessionFolder);
    printf(" Project Tempo:     %.0f BPM | Metronome: [%-3s]                   \n", pSystem->bpm, pSystem->metronomeOn ? "ON" : "OFF");
    printf("-------------------------------------------------------------------\n");
    printf("  # | Track Name           | Vol  | Status                         \n");
    printf("-------------------------------------------------------------------\n");

    for (int i = 0; i < MAX_TRACKS; i++) {
        char cursor = (pSystem->selectedTrack == i) ? '>' : ' ';
        char recSymbol[32] = "      ";
        
        if (pSystem->activeTrackIndex == i) {
            strcpy(recSymbol, "[REC] ");
        } else if (pSystem->hasRecordedData[i]) {
            strcpy(recSymbol, "READY ");
        }

        printf(" %c%d | %-20s | %.2f | %-30s\n", 
               cursor, 
               i + 1, 
               pSystem->trackNames[i], 
               pSystem->trackVolumes[i], 
               recSymbol);
    }

    printf("-------------------------------------------------------------------\n");
    printf(" CONTROLS:                                                         \n");
    printf("  [1-8 / Arrows] Select Track  | [SPACE] Start/Stop Recording (Retry)\n");
    printf("  [M] Toggle Metronome         | [Q] Stop & Export Master Mix        \n");
    printf("-------------------------------------------------------------------\n");
    printf(" STATUS: %-58s\n", pSystem->statusMessage);
    printf("===================================================================\n");
    fflush(stdout);
}

void start_recording_on_track(MultitrackSystem* pSystem, int trackIdx, ma_encoder_config* pEncConfig) {
    if (pSystem->activeTrackIndex != -1) {
        int prev = pSystem->activeTrackIndex;
        pSystem->activeTrackIndex = -1;
        if (pSystem->isTrackInitialized[prev]) {
            ma_encoder_uninit(&pSystem->encoders[prev]);
            pSystem->isTrackInitialized[prev] = MA_FALSE;
        }
        if (pSystem->hasRecordedData[prev]) {
            ma_decoder_init_file(pSystem->trackPaths[prev], NULL, &pSystem->decoders[prev]);
        }
    }

    if (strcmp(pSystem->trackNames[trackIdx], "[Empty Slot]") == 0) {
        snprintf(pSystem->trackNames[trackIdx], sizeof(pSystem->trackNames[trackIdx]), "track_%d.wav", trackIdx + 1);
    }
    
    snprintf(pSystem->trackPaths[trackIdx], sizeof(pSystem->trackPaths[trackIdx]), 
             "%s/%s", pSystem->sessionFolder, pSystem->trackNames[trackIdx]);

    if (pSystem->hasRecordedData[trackIdx]) {
        ma_decoder_uninit(&pSystem->decoders[trackIdx]);
        pSystem->hasRecordedData[trackIdx] = MA_FALSE;
    }
    if (pSystem->isTrackInitialized[trackIdx]) {
        ma_encoder_uninit(&pSystem->encoders[trackIdx]);
        pSystem->isTrackInitialized[trackIdx] = MA_FALSE;
    }

    pSystem->sampleCounter = 0;

    if (ma_encoder_init_file(pSystem->trackPaths[trackIdx], pEncConfig, &pSystem->encoders[trackIdx]) == MA_SUCCESS) {
        pSystem->isTrackInitialized[trackIdx] = MA_TRUE;
        pSystem->activeTrackIndex = trackIdx;
        snprintf(pSystem->statusMessage, sizeof(pSystem->statusMessage), "RECORDING Track %d (%s)... Press SPACE to stop.", trackIdx + 1, pSystem->trackNames[trackIdx]);
    } else {
        snprintf(pSystem->statusMessage, sizeof(pSystem->statusMessage), "ERROR: Could not create file %s", pSystem->trackPaths[trackIdx]);
    }
}

void stop_recording(MultitrackSystem* pSystem) {
    int active = pSystem->activeTrackIndex;
    if (active != -1) {
        pSystem->activeTrackIndex = -1;
        
        if (pSystem->isTrackInitialized[active]) {
            ma_encoder_uninit(&pSystem->encoders[active]);
            pSystem->isTrackInitialized[active] = MA_FALSE;
        }

        if (pSystem->hasRecordedData[active]) {
            ma_decoder_init_file(pSystem->trackPaths[active], NULL, &pSystem->decoders[active]);
            snprintf(pSystem->statusMessage, sizeof(pSystem->statusMessage), "Stopped recording Track %d. Ready for playback/retry.", active + 1);
        } else {
            snprintf(pSystem->statusMessage, sizeof(pSystem->statusMessage), "Recording cancelled on Track %d.", active + 1);
        }
    }
}

int main(int argc, char** argv)
{
    MultitrackSystem trackSystem;
    trackSystem.selectedTrack = 0;
    trackSystem.activeTrackIndex = -1;
    trackSystem.metronomeOn = MA_TRUE;
    trackSystem.sampleCounter = 0;

    snprintf(trackSystem.sessionFolder, sizeof(trackSystem.sessionFolder), "my_session");
    trackSystem.bpm = 120.0f;
    snprintf(trackSystem.statusMessage, sizeof(trackSystem.statusMessage), "Studio Standby. Press SPACE to start recording.");

    if (argc > 1) snprintf(trackSystem.sessionFolder, sizeof(trackSystem.sessionFolder), "%s", argv[1]);
    if (argc > 2) {
        float parsedBpm = (float)atof(argv[2]);
        if (parsedBpm > 0.0f) trackSystem.bpm = parsedBpm;
    }

    MKDIR(trackSystem.sessionFolder);

    char masterFilePath[512];
    snprintf(masterFilePath, sizeof(masterFilePath), "%s/master_mix.wav", trackSystem.sessionFolder);

    trackSystem.samplesPerBeat = (ma_uint64)((60.0f / trackSystem.bpm) * SAMPLE_RATE);
    trackSystem.clickSampleLength = (int)(SAMPLE_RATE * 0.05f);

    for (int i = 0; i < MAX_TRACKS; i++) {
        trackSystem.isTrackInitialized[i] = MA_FALSE;
        trackSystem.hasRecordedData[i] = MA_FALSE;
        trackSystem.trackVolumes[i] = 0.8f;
        strcpy(trackSystem.trackNames[i], "[Empty Slot]");
        trackSystem.trackPaths[i][0] = '\0';
    }

    ma_encoder_config encConfig = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 2, SAMPLE_RATE);

    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_duplex);
    deviceConfig.capture.format   = ma_format_f32;
    deviceConfig.capture.channels = 2;
    deviceConfig.playback.format  = ma_format_f32;
    deviceConfig.playback.channels = 2;
    deviceConfig.sampleRate       = SAMPLE_RATE;
    deviceConfig.dataCallback     = data_callback;
    deviceConfig.pUserData        = &trackSystem;

    ma_device device;
    if (ma_device_init(NULL, &deviceConfig, &device) != MA_SUCCESS) {
        printf("Failed to boot audio hardware engine.\n");
        return -1;
    }
    ma_device_start(&device);

#ifndef _WIN32
    set_terminal_raw(1);
#endif

    int running = 1;
    while (running) {
        // Redraw UI immediately before waiting for input
        render_ui(&trackSystem);

        // Blocking read: Thread sleeps here until a key is pressed
        int ch = READ_KEY();

        if (ch == 0 || ch == 224 || ch == 27) {
            // Arrow key handling
            ch = READ_KEY();
            if (ch == '[') ch = READ_KEY(); // Handle ESC sequence on Linux/macOS
            
            if (ch == 'A' || ch == 72) { // UP
                if (trackSystem.selectedTrack > 0) trackSystem.selectedTrack--;
            } else if (ch == 'B' || ch == 80) { // DOWN
                if (trackSystem.selectedTrack < MAX_TRACKS - 1) trackSystem.selectedTrack++;
            }
        } 
        else if (ch >= '1' && ch <= '8') {
            trackSystem.selectedTrack = ch - '1';
        } 
        else if (ch == ' ') { // SPACE BAR
            if (trackSystem.activeTrackIndex == trackSystem.selectedTrack) {
                stop_recording(&trackSystem);
            } else {
                start_recording_on_track(&trackSystem, trackSystem.selectedTrack, &encConfig);
            }
        } 
        else if (ch == 'm' || ch == 'M') {
            trackSystem.metronomeOn = !trackSystem.metronomeOn;
        } 
        else if (ch == 'q' || ch == 'Q') {
            if (trackSystem.activeTrackIndex != -1) {
                stop_recording(&trackSystem);
            }
            running = 0;
        }
    }

#ifndef _WIN32
    set_terminal_raw(0);
#endif

    // Exit UI cleanly
    printf("\n\nStopping hardware audio engine...\n");
    ma_device_uninit(&device);

    for (int i = 0; i < MAX_TRACKS; i++) {
        if (trackSystem.isTrackInitialized[i]) {
            ma_encoder_uninit(&trackSystem.encoders[i]);
            trackSystem.isTrackInitialized[i] = MA_FALSE; 
        }
    }

    // Mix tracks to master track
    printf("MASTER MIXING:\n\n");

    ma_uint64 maxTotalFrames = 0;
    int activeDecodersCount = 0;

    for (int i = 0; i < MAX_TRACKS; i++) {
        if (trackSystem.hasRecordedData[i]) {
            ma_decoder_uninit(&trackSystem.decoders[i]); 
            if (ma_decoder_init_file(trackSystem.trackPaths[i], NULL, &trackSystem.decoders[i]) == MA_SUCCESS) {
                activeDecodersCount++;
                ma_uint64 totalFrames = 0;
                ma_decoder_get_length_in_pcm_frames(&trackSystem.decoders[i], &totalFrames);
                if (totalFrames > maxTotalFrames) {
                    maxTotalFrames = totalFrames;
                }
            }
        }
    }

    if (activeDecodersCount > 0 && maxTotalFrames > 0) {
        printf("\nAdjust final track volumes (Default: 0.8, Mute: 0.0, Max: 1.0+):\n");
        for (int i = 0; i < MAX_TRACKS; i++) {
            if (trackSystem.hasRecordedData[i]) {
                printf("  Vol for %-15s [Default: %.2f] -> Enter new vol (-1 to keep): ", 
                       trackSystem.trackNames[i], trackSystem.trackVolumes[i]);
                float userVol = -1.0f;
                if (scanf("%f", &userVol) == 1 && userVol >= 0.0f) {
                    trackSystem.trackVolumes[i] = userVol;
                }
            }
        }

        ma_encoder masterEncoder;
        ma_encoder_config masterConfig = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 2, SAMPLE_RATE);

        if (ma_encoder_init_file(masterFilePath, &masterConfig, &masterEncoder) == MA_SUCCESS) {
            printf("\nBouncing stems into '%s'...\n", masterFilePath);

            #define CHUNK_SIZE 512
            float mixChunk[CHUNK_SIZE * 2];
            float readChunk[CHUNK_SIZE * 2];
            ma_uint64 totalFramesProcessed = 0;

            while (totalFramesProcessed < maxTotalFrames) {
                ma_uint64 framesToProcess = maxTotalFrames - totalFramesProcessed;
                if (framesToProcess > CHUNK_SIZE) framesToProcess = CHUNK_SIZE;

                memset(mixChunk, 0, sizeof(float) * framesToProcess * 2);

                for (int t = 0; t < MAX_TRACKS; t++) {
                    if (trackSystem.hasRecordedData[t]) {
                        ma_uint64 framesRead = 0;
                        ma_decoder_read_pcm_frames(&trackSystem.decoders[t], readChunk, framesToProcess, &framesRead);

                        float vol = trackSystem.trackVolumes[t];
                        for (ma_uint64 i = 0; i < framesRead * 2; i++) {
                            mixChunk[i] += readChunk[i] * vol;
                        }
                    }
                }

                ma_encoder_write_pcm_frames(&masterEncoder, mixChunk, framesToProcess, NULL);
                totalFramesProcessed += framesToProcess;
            }

            ma_encoder_uninit(&masterEncoder);
            printf("\n>> Master Mixdown complete! Exported to: %s <<\n", masterFilePath);
        } else {
            printf("Error: Could not allocate file target for master export.\n");
        }
    } else {
        printf("No tracks were recorded. Skipping master mix bounce.\n");
    }

    for (int i = 0; i < MAX_TRACKS; i++) {
        if (trackSystem.hasRecordedData[i]) {
            ma_decoder_uninit(&trackSystem.decoders[i]);
        }
    }

    printf("Studio shutdown complete.\n");
    return 0;
}