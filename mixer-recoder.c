#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define MAX_TRACKS 8
#define SAMPLE_RATE 48000
#define PI 3.14159265358979323846f

typedef struct {
    ma_encoder encoders[MAX_TRACKS];
    ma_decoder decoders[MAX_TRACKS];
    ma_bool32   isTrackInitialized[MAX_TRACKS];
    ma_bool32   hasRecordedData[MAX_TRACKS];
    char        trackNames[MAX_TRACKS][256];
    float       trackVolumes[MAX_TRACKS]; // Individual gain per track (default: 0.8)
    int         activeTrackIndex;
    
    // Metronome Variables
    ma_bool32   metronomeOn;
    float       bpm;
    ma_uint64   sampleCounter;
    ma_uint64   samplesPerBeat;
    int         clickSampleLength;
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
    if (pInput != NULL && currentTrack >= 0 && currentTrack < MAX_TRACKS && pSystem->isTrackInitialized[currentTrack]) {
        ma_encoder_write_pcm_frames(&pSystem->encoders[currentTrack], pInput, frameCount, NULL);
        pSystem->hasRecordedData[currentTrack] = MA_TRUE; 
    }

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

int main(int argc, char** argv)
{
    MultitrackSystem trackSystem;
    trackSystem.activeTrackIndex = -1;
    trackSystem.metronomeOn = MA_TRUE;
    trackSystem.sampleCounter = 0;

    // Default configuration values
    char masterFilename[256] = "master_mix.wav";
    trackSystem.bpm = 120.0f;

    // Parse Command Line Arguments
    if (argc > 1) {
        // Ensure .wav extension is present or appends automatically
        if (strstr(argv[1], ".wav") == NULL) {
            snprintf(masterFilename, sizeof(masterFilename), "%s.wav", argv[1]);
        } else {
            snprintf(masterFilename, sizeof(masterFilename), "%s", argv[1]);
        }
    }
    if (argc > 2) {
        float parsedBpm = (float)atof(argv[2]);
        if (parsedBpm > 0.0f) {
            trackSystem.bpm = parsedBpm;
        }
    }

    trackSystem.samplesPerBeat = (ma_uint64)((60.0f / trackSystem.bpm) * SAMPLE_RATE);
    trackSystem.clickSampleLength = (int)(SAMPLE_RATE * 0.05f);

    // Track array flags & default volume initialization (0.8f default)
    for (int i = 0; i < MAX_TRACKS; i++) {
        trackSystem.isTrackInitialized[i] = MA_FALSE;
        trackSystem.hasRecordedData[i] = MA_FALSE;
        trackSystem.trackVolumes[i] = 0.8f;
        strcpy(trackSystem.trackNames[i], "[Empty Slot]");
    }

    printf("==========================================\n");
    printf(" Multitrack Recorder Session Started\n");
    printf(" Target Master File: %s\n", masterFilename);
    printf(" Project Tempo:      %.1f BPM\n", trackSystem.bpm);
    printf(" Default Stem Vol:   0.8\n");
    printf("==========================================\n");

    ma_encoder_config encConfig = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 2, SAMPLE_RATE);

    // Boot Duplex engine
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
        printf("Failed to boot hardware engine.\n");
        return -1;
    }
    ma_device_start(&device);

    int running = 1;
    while (running) {
        printf("\n--- Recorder ---\n");
        printf("Track List Status:\n");
        for(int i = 0; i < MAX_TRACKS; i++) {
            printf("  Track %d: %-20s (Vol: %.2f) %s\n", 
                   i + 1, 
                   trackSystem.trackNames[i], 
                   trackSystem.trackVolumes[i],
                   (trackSystem.activeTrackIndex == i) ? "<< RECORDING NOW <<" : "");
        }
        printf("\nMetronome: %s (%0.0f BPM)\n", trackSystem.metronomeOn ? "ON" : "OFF", trackSystem.bpm);
        printf("Controls: \n  1-8 -> Record Track | m -> Toggle Click | s -> Standby | q -> Save and Quit\nSelection: ");
        
        char input;
        scanf(" %c", &input);

        if (input >= '1' && input <= '8') {
            int selected = input - '1';
            char inputName[200];

            printf("Enter file name for Track %d (e.g., guitar, vocals) without extension: ", selected + 1);
            scanf("%s", inputName);
            
            char fullPath[256];
            sprintf(fullPath, "%s.wav", inputName);

            if (trackSystem.hasRecordedData[selected]) {
                ma_decoder_uninit(&trackSystem.decoders[selected]);
                trackSystem.hasRecordedData[selected] = MA_FALSE;
            }
            if (trackSystem.isTrackInitialized[selected]) {
                ma_encoder_uninit(&trackSystem.encoders[selected]);
                trackSystem.isTrackInitialized[selected] = MA_FALSE;
            }

            if (ma_encoder_init_file(fullPath, &encConfig, &trackSystem.encoders[selected]) == MA_SUCCESS) {
                trackSystem.isTrackInitialized[selected] = MA_TRUE;
                strcpy(trackSystem.trackNames[selected], fullPath);
                
                trackSystem.activeTrackIndex = selected;
                printf(">> Recording initialized on hardware layer. Target: %s <<\n", fullPath);
            } else {
                printf("Error: Could not open file %s for writing.\n", fullPath);
            }
        }
        else if (input == 's' || input == 'S') {
            int previous = trackSystem.activeTrackIndex;
            trackSystem.activeTrackIndex = -1;
            
            if (previous != -1 && trackSystem.hasRecordedData[previous]) {
                ma_decoder_init_file(trackSystem.trackNames[previous], NULL, &trackSystem.decoders[previous]);
            }
            printf(">> Studio in Standby <<\n");
        }
        else if (input == 'm' || input == 'M') {
            trackSystem.metronomeOn = !trackSystem.metronomeOn;
        }
        else if (input == 'q' || input == 'Q') {
            running = 0;
        }
    }

    // Safe Studio Shutdown
    printf("\nStopping hardware audio engine pipeline...\n");
    ma_device_uninit(&device);

    for (int i = 0; i < MAX_TRACKS; i++) {
        if (trackSystem.isTrackInitialized[i]) {
            ma_encoder_uninit(&trackSystem.encoders[i]);
            trackSystem.isTrackInitialized[i] = MA_FALSE; 
        }
    }

    // --- MASTER MIXDOWN STAGE ---
    printf("\n--- INITIALIZING MASTER MIXDOWN ---\n");

    ma_uint64 maxTotalFrames = 0;
    int activeDecodersCount = 0;

    for (int i = 0; i < MAX_TRACKS; i++) {
        if (trackSystem.hasRecordedData[i]) {
            ma_decoder_uninit(&trackSystem.decoders[i]); 
            if (ma_decoder_init_file(trackSystem.trackNames[i], NULL, &trackSystem.decoders[i]) == MA_SUCCESS) {
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
        // Track Volume Prompting Stage
        printf("\nAdjust volumes for mixing (Default: 0.8, Silence: 0.0, Unity Gain: 1.0):\n");
        for (int i = 0; i < MAX_TRACKS; i++) {
            if (trackSystem.hasRecordedData[i]) {
                printf("Volume for %s [Current: %.2f] (Enter -1 to keep current): ", trackSystem.trackNames[i], trackSystem.trackVolumes[i]);
                float userVol = -1.0f;
                if (scanf("%f", &userVol) == 1 && userVol >= 0.0f) {
                    trackSystem.trackVolumes[i] = userVol;
                }
            }
        }

        ma_encoder masterEncoder;
        ma_encoder_config masterConfig = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 2, SAMPLE_RATE);

        if (ma_encoder_init_file(masterFilename, &masterConfig, &masterEncoder) == MA_SUCCESS) {
            printf("\nBouncing stems into '%s' (%llu total frames)...\n", masterFilename, maxTotalFrames);

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
            printf(">> Master Mixdown complete! Saved to '%s' <<\n", masterFilename);
        } else {
            printf("Error: Could not allocate memory or file target for master export.\n");
        }
    } else {
        printf("No tracks were recorded. Skipping master mix creation.\n");
    }

    for (int i = 0; i < MAX_TRACKS; i++) {
        if (trackSystem.hasRecordedData[i]) {
            ma_decoder_uninit(&trackSystem.decoders[i]);
        }
    }

    printf("Studio shutdown complete.\n");
    return 0;
}

