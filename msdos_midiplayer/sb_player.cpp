/*
 * Sound Blaster WAV Player Implementation
 * Adapted for integration with the MIDI player
 * With BLASTER environment variable support
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include <conio.h>
#include <go32.h>
#include <dpmi.h>
#include <sys/nearptr.h>
#include <pc.h>
#include "sb_player.h"
#include "dos_utils.h"

// Sound Blaster settings - defaults that can be overridden by BLASTER env var
static int sb_base = 0x220;     // Default I/O port 
static int sb_irq = 7;          // Default IRQ
static int sb_dma = 1;          // Default 8-bit DMA channel
static int sb_hdma = 5;         // Default 16-bit DMA channel

// Define I/O port addresses based on base port
#define SB_RESET (sb_base + 0x6)
#define SB_READ (sb_base + 0xA)
#define SB_WRITE (sb_base + 0xC)
#define SB_STATUS (sb_base + 0xE)
#define SB_MIXER_ADDR (sb_base + 0x4)
#define SB_MIXER_DATA (sb_base + 0x5)

// Default DMA buffer size - will be adjusted based on audio format
static int dma_buffer_size = 8192;

// Double buffer structure
typedef struct {
    _go32_dpmi_seginfo segment;
    void *address;
    int size;
    int active;                 // 0 or 1
} DMA_BUFFER;

// Global variables
static DMA_BUFFER dma_buffers[2];
static FILE *wav_file = NULL;
static unsigned long data_size = 0;
static unsigned long bytes_read = 0;
static volatile int playing = 0;
static unsigned short sample_rate = 22050;
static unsigned short channels = 1;
static unsigned short bits_per_sample = 8;
static DSP_VERSION dsp_version = {0, 0};
static int using_16bit = 0;
static volatile int current_buffer = 0;

// Parse BLASTER environment variable
static int parse_blaster_env(void) {
    char *blaster = getenv("BLASTER");
    if (!blaster) {
        // No BLASTER environment variable, use defaults
        return 1;
    }
    
    printf("BLASTER environment: %s\n", blaster);
    
    // Parse BLASTER environment variable
    // Format is typically: A220 I7 D1 H5 P330 T6
    char *p = blaster;
    while (*p) {
        switch (*p) {
            case 'A': case 'a':
                // I/O port address
                p++;
                sb_base = strtol(p, &p, 16);
                break;
            
            case 'I': case 'i':
                // IRQ number
                p++;
                sb_irq = strtol(p, &p, 10);
                break;
            
            case 'D': case 'd':
                // 8-bit DMA channel
                p++;
                sb_dma = strtol(p, &p, 10);
                break;
            
            case 'H': case 'h':
                // 16-bit DMA channel
                p++;
                sb_hdma = strtol(p, &p, 10);
                break;
            
            default:
                // Skip other parameters
                p++;
                while (*p && *p != ' ') p++;
                break;
        }
        
        // Skip spaces
        while (*p == ' ') p++;
    }
    
    printf("Sound Blaster config: Port 0x%x, IRQ %d, DMA %d, HDMA %d\n", 
           sb_base, sb_irq, sb_dma, sb_hdma);
    
    return 1;
}

// Reset the Sound Blaster DSP
static int reset_dsp(void) {
    outportb(SB_RESET, 1);
    delay(10);
    outportb(SB_RESET, 0);
    delay(10);
    
    int timeout = 100;
    while (timeout-- && !(inportb(SB_STATUS) & 0x80))
        delay(1);
    
    if (timeout <= 0) {
        printf("DSP reset timeout\n");
        return 0;
    }
    
    int val = inportb(SB_READ);
    if (val != 0xAA) {
        printf("DSP reset failed: got 0x%02X instead of 0xAA\n", val);
        return 0;
    }
    
    return 1;
}

// Write a command to the DSP
static void write_dsp(unsigned char value) {
    int timeout = 1000;
    while (timeout-- && (inportb(SB_STATUS) & 0x80))
        delay(1);
    
    if (timeout <= 0) {
        printf("DSP write timeout\n");
        return;
    }
    
    outportb(SB_WRITE, value);
}

// Get DSP version
static int get_dsp_version(DSP_VERSION *version) {
    write_dsp(0xE1);
    
    int timeout = 100;
    while (timeout-- && !(inportb(SB_STATUS) & 0x80))
        delay(1);
    
    if (timeout <= 0)
        return 0;
    
    version->major = inportb(SB_READ);
    
    timeout = 100;
    while (timeout-- && !(inportb(SB_STATUS) & 0x80))
        delay(1);
    
    if (timeout <= 0)
        return 0;
    
    version->minor = inportb(SB_READ);
    
    return 1;
}

// Set the DSP sample rate for playback
static void set_sample_rate(unsigned short rate) {
    if (using_16bit) {
        // Set 16-bit sample rate
        write_dsp(0x41);  // Set output sample rate
        write_dsp(rate >> 8);
        write_dsp(rate & 0xFF);
    } else {
        // Set 8-bit sample rate
        write_dsp(0x40);
        unsigned char time_constant = 256 - (1000000 / rate);
        write_dsp(time_constant);
    }
    
    // Introduce a small delay after setting sample rate
    delay(10);
}

// Calculate optimal buffer size for audio format
static int calculate_optimal_buffer_size(unsigned short channels, 
                                       unsigned short bits_per_sample, 
                                       unsigned long sample_rate) {
    // Calculate bytes per second
    unsigned long bytes_per_sec = sample_rate * channels * (bits_per_sample / 8);
    
    // Base buffer sizes for different bit rates
    int buffer_size;
    
    if (bytes_per_sec <= 22050) {
        // Low quality audio (e.g., 11025 Hz, 8-bit, mono)
        buffer_size = 4096;  // ~0.2 seconds
    } else if (bytes_per_sec <= 88200) {
        // Medium quality (e.g., 22050 Hz, 16-bit, stereo)
        buffer_size = 8192;  // ~0.1 seconds
    } else {
        // High quality (48000+ Hz, 16-bit, stereo)
        buffer_size = 32768;  // ~0.1 seconds
    }
    
    // Ensure buffer size doesn't exceed maximum safe DMA boundary
    if (buffer_size > 65536 - 16) {
        buffer_size = 65536 - 16;
    }
    
    // For 16-bit samples, ensure buffer size is even
    if (bits_per_sample == 16) {
        buffer_size &= ~1;
    }
    
    // For stereo, ensure buffer size is multiple of 2*bytes_per_sample
    if (channels == 2) {
        int bytes_per_frame = channels * (bits_per_sample / 8);
        buffer_size = (buffer_size / bytes_per_frame) * bytes_per_frame;
    }
    
    return buffer_size;
}

// Allocate DMA buffers for double-buffering
static int allocate_dma_buffers(void) {
    int i;
    for (i = 0; i < 2; i++) {
        // Allocate DOS memory block
        dma_buffers[i].segment.size = (dma_buffer_size + 15) / 16;  // Size in paragraphs
        
        if (_go32_dpmi_allocate_dos_memory(&dma_buffers[i].segment) != 0) {
            // Fail - free any allocated buffers
            while (--i >= 0) {
                _go32_dpmi_free_dos_memory(&dma_buffers[i].segment);
            }
            printf("Failed to allocate DMA buffer %d\n", i);
            return 0;
        }
        
        // Calculate the linear address
        dma_buffers[i].address = (void *)((dma_buffers[i].segment.rm_segment << 4) + __djgpp_conventional_base);
        dma_buffers[i].size = dma_buffer_size;
        dma_buffers[i].active = 0;
    }
    
    return 1;
}

// Free DMA buffers
static void free_dma_buffers(void) {
    for (int i = 0; i < 2; i++) {
        if (dma_buffers[i].segment.size > 0) {
            _go32_dpmi_free_dos_memory(&dma_buffers[i].segment);
            dma_buffers[i].segment.size = 0;
        }
    }
}

// Program the DMA controller for the current buffer
static void program_dma(int buffer_index) {
    unsigned long phys_addr = dma_buffers[buffer_index].segment.rm_segment << 4;
    unsigned char page;
    unsigned short offset;
    int count = dma_buffers[buffer_index].size;
    
    if (using_16bit) {
        // For 16-bit DMA
        page = phys_addr >> 16;
        offset = (phys_addr >> 1) & 0xFFFF;  // 16-bit DMA uses word count
        
        // Disable DMA channel
        outportb(0xD4, (sb_hdma & 3) | 4);
        
        // Clear flip-flop
        outportb(0xD8, 0);
        
        // Set mode (single cycle, write, channel)
        outportb(0xD6, 0x48 | (sb_hdma & 3));
        
        // Set DMA address
        outportb(0xC0 + ((sb_hdma & 3) << 2), offset & 0xFF);
        outportb(0xC0 + ((sb_hdma & 3) << 2), offset >> 8);
        
        // Set DMA page
        switch (sb_hdma & 3) {
            case 0: outportb(0x87, page); break;  // DMA 4
            case 1: outportb(0x83, page); break;  // DMA 5
            case 2: outportb(0x81, page); break;  // DMA 6
            case 3: outportb(0x82, page); break;  // DMA 7
        }
        
        // Set DMA count (bytes - 1) for 16-bit DMA
        count = count / 2;  // Convert to 16-bit word count
        outportb(0xC2 + ((sb_hdma & 3) << 2), (count - 1) & 0xFF);
        outportb(0xC2 + ((sb_hdma & 3) << 2), (count - 1) >> 8);
        
        // Enable DMA channel
        outportb(0xD4, sb_hdma & 3);
    } else {
        // For 8-bit DMA
        page = phys_addr >> 16;
        offset = phys_addr & 0xFFFF;
        
        // Disable DMA channel
        outportb(0x0A, sb_dma | 4);
        
        // Clear flip-flop
        outportb(0x0C, 0);
        
        // Set mode (single cycle, write, channel)
        outportb(0x0B, 0x48 | sb_dma);
        
        // Set DMA address
        outportb(sb_dma << 1, offset & 0xFF);
        outportb(sb_dma << 1, offset >> 8);
        
        // Set DMA page - pages for 8-bit DMA channels
        switch (sb_dma) {
            case 0: outportb(0x87, page); break;  // DMA 0
            case 1: outportb(0x83, page); break;  // DMA 1
            case 2: outportb(0x81, page); break;  // DMA 2
            case 3: outportb(0x82, page); break;  // DMA 3
        }
        
        // Set DMA count (bytes - 1)
        outportb((sb_dma << 1) + 1, (count - 1) & 0xFF);
        outportb((sb_dma << 1) + 1, (count - 1) >> 8);
        
        // Enable DMA channel
        outportb(0x0A, sb_dma);
    }
}

// Fill a DMA buffer with audio data
static int fill_buffer(int buffer_index) {
    int bytes_to_read = dma_buffers[buffer_index].size;
    void *buffer_addr = dma_buffers[buffer_index].address;
    
    // Adjust bytes to read if near end of file
    if (bytes_read + bytes_to_read > data_size) {
        bytes_to_read = data_size - bytes_read;
        if (bytes_to_read <= 0) {
            return 0;  // End of file
        }
    }
    
    // Read data into buffer - fix for comparison warning
    size_t bytes_actually_read = fread(buffer_addr, 1, bytes_to_read, wav_file);
    if (bytes_actually_read != (size_t)bytes_to_read) {
        printf("Error reading WAV data (expected %d, got %lu bytes)\n", 
               bytes_to_read, (unsigned long)bytes_actually_read);
        return 0;
    }
    
    // Pad with silence if not full
    if (bytes_to_read < dma_buffers[buffer_index].size) {
        if (bits_per_sample == 8) {
            // 8-bit silence is 128 (unsigned)
            memset((char *)buffer_addr + bytes_to_read, 128, 
                   dma_buffers[buffer_index].size - bytes_to_read);
        } else {
            // 16-bit silence is 0 (signed)
            memset((char *)buffer_addr + bytes_to_read, 0, 
                   dma_buffers[buffer_index].size - bytes_to_read);
        }
    }
    
    // Update bytes read counter
    bytes_read += bytes_to_read;
    
    return 1;  // Success
}

// Start playback of a buffer
static void start_buffer_playback(int buffer_index) {
    // Program DMA controller
    program_dma(buffer_index);
    
    // Start playback
    if (using_16bit) {
        if (channels == 2) {
            // 16-bit stereo
            write_dsp(0xB6);  // DSP command for 16-bit playback
            write_dsp(0x30);  // Mode: signed stereo
            
            // For stereo, count is number of frames (stereo pairs)
            int count = dma_buffers[buffer_index].size / 4;  // 4 bytes per frame
            write_dsp((count - 1) & 0xFF);
            write_dsp((count - 1) >> 8);
        } else {
            // 16-bit mono
            write_dsp(0xB6);  // DSP command for 16-bit playback
            write_dsp(0x10);  // Mode: signed mono
            
            int count = dma_buffers[buffer_index].size / 2;  // 2 bytes per sample
            write_dsp((count - 1) & 0xFF);
            write_dsp((count - 1) >> 8);
        }
    } else {
        if (channels == 2) {
            // 8-bit stereo
            write_dsp(0xC6);  // DSP command for 8-bit playback
            write_dsp(0x20);  // Mode: unsigned stereo
            
            int count = dma_buffers[buffer_index].size / 2;  // 2 bytes per frame
            write_dsp((count - 1) & 0xFF);
            write_dsp((count - 1) >> 8);
        } else {
            // 8-bit mono
            write_dsp(0xC6);  // DSP command for 8-bit playback
            write_dsp(0x00);  // Mode: unsigned mono
            
            write_dsp((dma_buffers[buffer_index].size - 1) & 0xFF);
            write_dsp((dma_buffers[buffer_index].size - 1) >> 8);
        }
    }
    
    // Mark buffer as active
    dma_buffers[buffer_index].active = 1;
}

// Calculate milliseconds needed to play a buffer
static int calculate_buffer_time(int buffer_size) {
    int bytes_per_sample = bits_per_sample / 8;
    int bytes_per_frame = bytes_per_sample * channels;
    return (buffer_size * 1000) / (sample_rate * bytes_per_frame);
}

// Check keyboard - this function actively polls for key presses
static int check_keyboard(void) {
    if (kbhit()) {
        int ch = getch();
        if (ch == 'q' || ch == 'Q' || ch == 27) {  // Q, q, or ESC
            return 1;  // Stop playback
        } else if (ch == ' ') {  // Space bar for pause
            printf("\nPlayback paused. Press any key to continue...\n");
            getch();  // Wait for any key press
            printf("Resuming playback...\n");
        }
    }
    return 0;  // Continue playback
}

// Play a WAV file
bool play_wav_file(const char *filename) {
    WAV_HEADER header;
    int buffer_time_ms;
    int end_of_file = 0;
    
    printf("Playing %s...\n", filename);
    
    // Parse BLASTER environment variable
    if (!parse_blaster_env()) {
        printf("Warning: Failed to parse BLASTER environment, using defaults\n");
    }
    
    // Enable near pointers
    if (__djgpp_nearptr_enable() == 0) {
        printf("Failed to enable near pointers\n");
        return false;
    }
    
    // Check SB version
    if (!reset_dsp() || !get_dsp_version(&dsp_version)) {
        printf("Failed to detect Sound Blaster at address 0x%x\n", sb_base);
        
        // Try the default address as a fallback
        sb_base = 0x220;
        if (!reset_dsp() || !get_dsp_version(&dsp_version)) {
            printf("Failed to detect Sound Blaster at fallback address 0x220\n");
            __djgpp_nearptr_disable();
            return false;
        }
        
        printf("Detected Sound Blaster at fallback address 0x220\n");
    }
    
    // Sound Blaster 16 and above supports 16-bit audio
    printf("Sound Blaster DSP version %d.%d detected\n", dsp_version.major, dsp_version.minor);
    
    if (dsp_version.major >= 4) {
        printf("16-bit audio support: Yes\n");
    } else {
        printf("16-bit audio support: No (will use 8-bit)\n");
    }
    
    // Open the WAV file
    wav_file = fopen(filename, "rb");
    if (!wav_file) {
        printf("Failed to open WAV file\n");
        __djgpp_nearptr_disable();
        return false;
    }
    
    // Read the WAV header
    if (fread(&header, sizeof(WAV_HEADER), 1, wav_file) != 1) {
        printf("Failed to read WAV header\n");
        fclose(wav_file);
        __djgpp_nearptr_disable();
        return false;
    }
    
    // Check if this is a valid WAV file
    if (memcmp(header.riff_header, "RIFF", 4) != 0 || 
        memcmp(header.wave_header, "WAVE", 4) != 0 ||
        memcmp(header.fmt_header, "fmt ", 4) != 0 ||
        memcmp(header.data_header, "data", 4) != 0) {
        printf("Not a valid WAV file structure\n");
        fclose(wav_file);
        __djgpp_nearptr_disable();
        return false;
    }
    
    // Check format - must be PCM
    if (header.audio_format != 1) {
        printf("Only PCM format (1) is supported, file has format %d\n", header.audio_format);
        fclose(wav_file);
        __djgpp_nearptr_disable();
        return false;
    }
    
    // Store format information
    channels = header.num_channels;
    sample_rate = header.sample_rate;
    bits_per_sample = header.bits_per_sample;
    data_size = header.data_size;
    
    // Display audio info
    printf("Audio: %dHz, %d-bit, %s\n", 
           sample_rate, bits_per_sample,
           (channels == 2) ? "stereo" : "mono");
    
    // Determine playback mode
    using_16bit = (bits_per_sample == 16) && (dsp_version.major >= 4);
    
    // Calculate optimal buffer size based on audio format
    dma_buffer_size = calculate_optimal_buffer_size(channels, bits_per_sample, sample_rate);
    
    // Calculate buffer playback time
    buffer_time_ms = calculate_buffer_time(dma_buffer_size);
        
    printf("Using %dK buffer, playback time: %d ms\n", 
           dma_buffer_size / 1024, buffer_time_ms);
    
    // Allocate DMA buffers
    if (!allocate_dma_buffers()) {
        printf("Failed to allocate DMA buffers\n");
        fclose(wav_file);
        __djgpp_nearptr_disable();
        return false;
    }
    
    // Reset the Sound Blaster
    if (!reset_dsp()) {
        printf("Failed to reset Sound Blaster\n");
        free_dma_buffers();
        fclose(wav_file);
        __djgpp_nearptr_disable();
        return false;
    }
    
    // Set the mixer for stereo or mono output
    outportb(SB_MIXER_ADDR, 0x0E);  // Select output control register
    if (channels == 2) {
        outportb(SB_MIXER_DATA, 0x03);  // Stereo (both left and right enabled)
    } else {
        outportb(SB_MIXER_DATA, 0x01);  // Mono (left only)
    }
    
    // Turn on the speaker
    write_dsp(0xD1);
    
    // Set the sample rate
    set_sample_rate(sample_rate);
    
    // Initialize bytes read counter
    bytes_read = 0;
    current_buffer = 0;
    
    // Fill both buffers to start
    if (!fill_buffer(0) || !fill_buffer(1)) {
        printf("Failed to fill initial buffers\n");
        free_dma_buffers();
        fclose(wav_file);
        __djgpp_nearptr_disable();
        return false;
    }
    
    printf("Playing WAV - press Q/ESC to stop, SPACE to pause...\n");
    
    // Start the first buffer
    start_buffer_playback(0);
    
    // Main playback loop using double-buffering
    playing = 1;
    keep_running = 1;
    
    // Playback loop
    while (playing && keep_running && !end_of_file) {
        // Set up variables for buffer timing
        int elapsed = 0;
        int next_buffer = (current_buffer + 1) % 2;
        int max_wait = buffer_time_ms * 0.9;  // Reduced wait time for better responsiveness
        bool buffer_wait_interrupted = false;
        
        // Check for key press before entering wait loop
        if (check_keyboard()) {
            playing = 0;
            break;
        }
        
        // Wait for the current buffer to finish, checking keys more frequently
        while (elapsed < max_wait && keep_running && !buffer_wait_interrupted) {
            // Check for key press every 5ms for responsive input
            if (check_keyboard()) {
                playing = 0;
                buffer_wait_interrupted = true;
            }
            
            // Only continue waiting if we haven't been interrupted
            if (!buffer_wait_interrupted) {
                delay(5);  // Short delay for more frequent checks
                elapsed += 5;
            }
        }
        
        // Only continue with next buffer if still playing
        if (playing) {
            // Switch buffers
            current_buffer = next_buffer;
            
            // Start playing the next buffer
            start_buffer_playback(current_buffer);
            
            // Fill the buffer that just finished playing
            next_buffer = (current_buffer + 1) % 2;
            if (!fill_buffer(next_buffer)) {
                end_of_file = 1;
            }
        }
    }
    
    // Stop playback
    write_dsp(0xD5);  // Stop Sound Blaster output
    delay(50);
    
    // Clear any key press from buffer
    while (kbhit()) getch();
    
    // Turn off the speaker
    write_dsp(0xD3);
    
    // Reset the DSP
    reset_dsp();
    
    // Close the file
    fclose(wav_file);
    wav_file = NULL;
    
    // Free DMA buffers
    free_dma_buffers();
    
    // Disable near pointers
    __djgpp_nearptr_disable();
    
    printf("Playback complete\n");
    return true;
}

// Check if Sound Blaster is present and get version
bool detect_sound_blaster(DSP_VERSION *version) {
    // Parse BLASTER environment variable first
    parse_blaster_env();
    
    if (!reset_dsp()) {
        // Try default address as fallback
        sb_base = 0x220;
        if (!reset_dsp()) {
            return false;
        }
    }
    
    return get_dsp_version(version);
}
