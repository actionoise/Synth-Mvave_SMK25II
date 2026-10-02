#include <alsa/asoundlib.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <csignal>
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <array>
#include <filesystem>
#include <iomanip>
#include <ctime>

constexpr int SAMPLE_RATE   = 48000;
constexpr int CHANNELS      = 2;
constexpr int BUFFER_FRAMES = 128;

constexpr int MAX_VOICES = 32;
constexpr int NUM_TRACKS = 7;
constexpr int MAX_SAMPLE_VOICES = 64;
constexpr int SAMPLE_INSTRUMENT = 18;

constexpr uint8_t LIVE_SOURCE = 255;

constexpr double PI = 3.14159265358979323846;

// -------------------------------------------------------
// STATO GENERALE
// -------------------------------------------------------

std::atomic<bool> running(true);
std::atomic<float> tremoloDepth(0.0f);

std::mutex voiceMutex;

// -------------------------------------------------------
// TEMPO
// -------------------------------------------------------

uint64_t millisNow()
{
    using namespace std::chrono;

    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

// -------------------------------------------------------
// STRUMENTI
// -------------------------------------------------------

const char* instrumentNames[] =
{
    "",
    "Square Retro",
    "Saw Synth",
    "Triangle",
    "Soft Sine",
    "Organ",
    "Bass",
    "Lead",
    "Pluck",
    "Bell",
    "Kick",
    "Snare",
    "Closed Hi-Hat",
    "Open Hi-Hat",
    "Tom",
    "Clap",
    "Drum Kit",
    "Reserved",
    "WAV Sample Kit"
};

int currentInstrument = 1;
std::array<uint8_t, 128> liveNoteInstrument{};

// Volume indipendente: 1.0 = 100%, 2.0 = 200%
std::atomic<float> instrumentVolume[19];

// -------------------------------------------------------
// VOCI
// -------------------------------------------------------

struct Voice
{
    bool active = false;

    uint8_t note = 0;
    uint8_t velocity = 0;
    uint8_t instrument = 1;
    uint8_t source = LIVE_SOURCE;

    double frequency = 440.0;
    double phase = 0.0;

    uint64_t startMs = 0;

    bool releasing = false;
    uint64_t releaseStartMs = 0;
};

Voice voices[MAX_VOICES];

// -------------------------------------------------------
// WAV SAMPLE KIT - STRUMENTO 18
// -------------------------------------------------------

enum class SampleMode
{
    OneShot,
    Gate,
    Loop
};

struct SampleData
{
    bool valid = false;
    std::string filename;
    SampleMode mode = SampleMode::OneShot;
    float volume = 1.0f;
    std::vector<float> left;
    std::vector<float> right;
};

struct SampleVoice
{
    bool active = false;
    uint8_t note = 0;
    uint8_t velocity = 0;
    uint8_t source = LIVE_SOURCE;
    const SampleData* sample = nullptr;
    size_t position = 0;
};

enum class OneShotLivePlaybackState
{
    Stopped,
    Playing,
    Paused
};

struct OneShotLiveState
{
    OneShotLivePlaybackState state = OneShotLivePlaybackState::Stopped;
    size_t position = 0;
    uint8_t velocity = 100;
    uint64_t lastPressMs = 0;
};

constexpr uint64_t ONESHOT_DOUBLE_PRESS_MS = 350;

std::array<SampleData, 128> sampleMap;
SampleVoice sampleVoices[MAX_SAMPLE_VOICES];
std::array<OneShotLiveState, 128> oneShotLiveStates;

std::string trim(const std::string& text)
{
    const char* ws = " \t\r\n";
    size_t a = text.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    size_t b = text.find_last_not_of(ws);
    return text.substr(a, b - a + 1);
}

uint16_t readLE16(std::istream& in)
{
    uint8_t b[2]{};
    in.read(reinterpret_cast<char*>(b), 2);
    return static_cast<uint16_t>(b[0] | (b[1] << 8));
}

uint32_t readLE32(std::istream& in)
{
    uint8_t b[4]{};
    in.read(reinterpret_cast<char*>(b), 4);
    return static_cast<uint32_t>(b[0]) |
           (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16) |
           (static_cast<uint32_t>(b[3]) << 24);
}

bool loadWav16(const std::string& path, SampleData& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        std::cerr << "[SAMPLE] File non trovato: " << path << std::endl;
        return false;
    }

    char riff[4]{};
    char wave[4]{};
    in.read(riff, 4);
    (void)readLE32(in);
    in.read(wave, 4);

    if (std::string(riff, 4) != "RIFF" || std::string(wave, 4) != "WAVE")
    {
        std::cerr << "[SAMPLE] WAV non valido: " << path << std::endl;
        return false;
    }

    uint16_t audioFormat = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    std::vector<int16_t> pcm;

    while (in && (!audioFormat || pcm.empty()))
    {
        char chunkIdChars[4]{};
        if (!in.read(chunkIdChars, 4)) break;
        uint32_t chunkSize = readLE32(in);
        std::string chunkId(chunkIdChars, 4);

        if (chunkId == "fmt ")
        {
            audioFormat = readLE16(in);
            channels = readLE16(in);
            sampleRate = readLE32(in);
            (void)readLE32(in); // byte rate
            (void)readLE16(in); // block align
            bitsPerSample = readLE16(in);
            if (chunkSize > 16)
                in.seekg(chunkSize - 16, std::ios::cur);
        }
        else if (chunkId == "data")
        {
            if (bitsPerSample != 16 || (channels != 1 && channels != 2))
            {
                in.seekg(chunkSize, std::ios::cur);
            }
            else
            {
                pcm.resize(chunkSize / sizeof(int16_t));
                in.read(reinterpret_cast<char*>(pcm.data()), chunkSize);
            }
        }
        else
        {
            in.seekg(chunkSize, std::ios::cur);
        }

        if (chunkSize & 1)
            in.seekg(1, std::ios::cur);
    }

    if (audioFormat != 1 || bitsPerSample != 16 || sampleRate == 0 ||
        (channels != 1 && channels != 2) || pcm.empty())
    {
        std::cerr << "[SAMPLE] Richiesto WAV PCM 16-bit mono/stereo: " << path << std::endl;
        return false;
    }

    const size_t inputFrames = pcm.size() / channels;
    if (inputFrames == 0) return false;

    std::vector<float> srcL(inputFrames);
    std::vector<float> srcR(inputFrames);

    for (size_t i = 0; i < inputFrames; ++i)
    {
        float l = pcm[i * channels] / 32768.0f;
        float r = (channels == 2) ? pcm[i * 2 + 1] / 32768.0f : l;
        srcL[i] = l;
        srcR[i] = r;
    }

    if (sampleRate == SAMPLE_RATE)
    {
        out.left = std::move(srcL);
        out.right = std::move(srcR);
    }
    else
    {
        const double ratio = static_cast<double>(sampleRate) / SAMPLE_RATE;
        const size_t outFrames = static_cast<size_t>(inputFrames / ratio);
        out.left.resize(std::max<size_t>(1, outFrames));
        out.right.resize(std::max<size_t>(1, outFrames));

        for (size_t i = 0; i < out.left.size(); ++i)
        {
            double srcPos = i * ratio;
            size_t i0 = static_cast<size_t>(srcPos);
            size_t i1 = std::min(i0 + 1, inputFrames - 1);
            double frac = srcPos - i0;
            out.left[i] = static_cast<float>(srcL[i0] * (1.0 - frac) + srcL[i1] * frac);
            out.right[i] = static_cast<float>(srcR[i0] * (1.0 - frac) + srcR[i1] * frac);
        }
    }

    out.valid = true;
    return true;
}

SampleMode parseSampleMode(const std::string& mode)
{
    if (mode == "gate") return SampleMode::Gate;
    if (mode == "loop") return SampleMode::Loop;
    return SampleMode::OneShot;
}

const char* sampleModeName(SampleMode mode)
{
    switch (mode)
    {
        case SampleMode::Gate: return "gate";
        case SampleMode::Loop: return "loop";
        default: return "oneshot";
    }
}

int loadSampleMap(const std::string& mapPath)
{
    for (auto& s : sampleMap)
        s = SampleData{};

    std::ifstream mapFile(mapPath);
    if (!mapFile)
    {
        std::cerr << "[SAMPLE] Impossibile aprire " << mapPath << std::endl;
        return 0;
    }

    std::filesystem::path baseDir = std::filesystem::path(mapPath).parent_path();
    std::string line;
    int loaded = 0;
    int lineNo = 0;

    while (std::getline(mapFile, line))
    {
        ++lineNo;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        int note = -1;
        try { note = std::stoi(trim(line.substr(0, eq))); }
        catch (...) { continue; }
        if (note < 0 || note > 127) continue;

        std::string rhs = trim(line.substr(eq + 1));
        if (rhs.empty() || rhs == "NONE" || rhs == "none") continue;

        std::stringstream ss(rhs);
        std::string filename, modeText, volumeText;
        std::getline(ss, filename, ',');
        std::getline(ss, modeText, ',');
        std::getline(ss, volumeText, ',');

        filename = trim(filename);
        modeText = trim(modeText);
        volumeText = trim(volumeText);

        if (filename.empty() || filename == "NONE" || filename == "none") continue;

        SampleData sample;
        sample.filename = filename;
        sample.mode = parseSampleMode(modeText);
        int volume = 100;
        if (!volumeText.empty())
        {
            try { volume = std::stoi(volumeText); }
            catch (...) { volume = 100; }
        }
        volume = std::clamp(volume, 0, 200);
        sample.volume = volume / 100.0f;

        std::filesystem::path filePath(filename);
        if (!filePath.is_absolute())
            filePath = baseDir / filePath;

        if (loadWav16(filePath.string(), sample))
        {
            sampleMap[note] = std::move(sample);
            ++loaded;
            std::cout << "[SAMPLE] Note " << note << " -> "
                      << sampleMap[note].filename << " ("
                      << sampleModeName(sampleMap[note].mode) << ", "
                      << static_cast<int>(std::lround(sampleMap[note].volume * 100.0f))
                      << "%)" << std::endl;
        }
        else
        {
            std::cerr << "[SAMPLE] Riga " << lineNo << " non caricata." << std::endl;
        }
    }

    std::cout << "[SAMPLE] " << loaded << " sample caricati in RAM." << std::endl;
    return loaded;
}

// -------------------------------------------------------
// REGISTRAZIONE DEL MIX FINALE IN WAV
// -------------------------------------------------------

std::mutex recorderMutex;
std::ofstream recordingFile;
bool audioRecording = false;
uint64_t recordedFrames = 0;
std::string currentRecordingPath;

void writeLE16(std::ostream& out, uint16_t v)
{
    char b[2] = { static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF) };
    out.write(b, 2);
}

void writeLE32(std::ostream& out, uint32_t v)
{
    char b[4] = {
        static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
        static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)
    };
    out.write(b, 4);
}

void writeRecordingHeader(std::ostream& out, uint32_t dataBytes)
{
    out.seekp(0, std::ios::beg);
    out.write("RIFF", 4);
    writeLE32(out, 36 + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeLE32(out, 16);
    writeLE16(out, 1); // PCM
    writeLE16(out, CHANNELS);
    writeLE32(out, SAMPLE_RATE);
    writeLE32(out, SAMPLE_RATE * CHANNELS * sizeof(int16_t));
    writeLE16(out, CHANNELS * sizeof(int16_t));
    writeLE16(out, 16);
    out.write("data", 4);
    writeLE32(out, dataBytes);
}

std::string recordingFilename()
{
    std::filesystem::create_directories("recordings");
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream os;
    os << "recordings/recording_" << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S") << ".wav";
    return os.str();
}

void startAudioRecording()
{
    std::lock_guard<std::mutex> lock(recorderMutex);
    if (audioRecording)
    {
        std::cout << "[RECORD] Registrazione gia attiva: " << currentRecordingPath << std::endl;
        return;
    }

    currentRecordingPath = recordingFilename();
    recordingFile.open(currentRecordingPath, std::ios::binary | std::ios::trunc);
    if (!recordingFile)
    {
        std::cerr << "[RECORD] Impossibile creare " << currentRecordingPath << std::endl;
        currentRecordingPath.clear();
        return;
    }

    writeRecordingHeader(recordingFile, 0);
    recordedFrames = 0;
    audioRecording = true;
    std::cout << "[RECORD] START -> " << currentRecordingPath << std::endl;
}

void stopAudioRecording()
{
    std::lock_guard<std::mutex> lock(recorderMutex);
    if (!audioRecording)
    {
        std::cout << "[RECORD] Nessuna registrazione attiva." << std::endl;
        return;
    }

    audioRecording = false;
    uint64_t bytes64 = recordedFrames * CHANNELS * sizeof(int16_t);
    uint32_t dataBytes = static_cast<uint32_t>(std::min<uint64_t>(bytes64, 0xFFFFFFFFu - 36u));
    writeRecordingHeader(recordingFile, dataBytes);
    recordingFile.flush();
    recordingFile.close();

    double seconds = static_cast<double>(recordedFrames) / SAMPLE_RATE;
    std::cout << "[RECORD] STOP/SAVE -> " << currentRecordingPath
              << " (" << std::fixed << std::setprecision(1) << seconds << " s)" << std::endl;
    std::cout.unsetf(std::ios::floatfield);
}

void appendRecording(const int16_t* buffer, size_t frames)
{
    std::lock_guard<std::mutex> lock(recorderMutex);
    if (!audioRecording || !recordingFile) return;
    recordingFile.write(reinterpret_cast<const char*>(buffer),
                        static_cast<std::streamsize>(frames * CHANNELS * sizeof(int16_t)));
    recordedFrames += frames;
}

// -------------------------------------------------------
// LOOPER
// -------------------------------------------------------

struct MidiEvent
{
    uint32_t timeMs;

    bool noteOn;

    uint8_t note;
    uint8_t velocity;
    uint8_t instrument;

    // Per lo strumento 18: posizione del WAV da cui ripartire.
    // Per gli strumenti synth resta sempre 0.
    size_t sampleStartPosition = 0;
};

struct LoopTrack
{
    std::vector<MidiEvent> events;

    bool recording = false;
    bool playing = false;

    uint64_t recordStart = 0;

    uint32_t duration = 0;

    uint64_t playStart = 0;
    uint32_t playhead = 0;

    uint32_t previousPos = 0;

    size_t playbackIndex = 0;
};

LoopTrack tracks[NUM_TRACKS];

// -------------------------------------------------------
// MIDI CONTROL MAP
// -------------------------------------------------------

const int REC_NOTES[NUM_TRACKS] =
{
    40, 41, 42, 43, 48, 49, 50
};

const int PLAY_NOTES[NUM_TRACKS] =
{
    36, 37, 38, 39, 44, 45, 46
};

constexpr int NEXT_INSTRUMENT_NOTE = 51;

// -------------------------------------------------------
// NOTE -> FREQUENZA
// -------------------------------------------------------

double noteToFrequency(int note)
{
    return 440.0 *
           std::pow(
               2.0,
               (note - 69) / 12.0
           );
}

// -------------------------------------------------------
// RUMORE
// -------------------------------------------------------

uint32_t noiseState = 0x12345678;

double noiseSample()
{
    noiseState ^= noiseState << 13;
    noiseState ^= noiseState >> 17;
    noiseState ^= noiseState << 5;

    return
        ((noiseState & 0xFFFF) / 32767.5)
        - 1.0;
}

// -------------------------------------------------------
// DRUM KIT
// -------------------------------------------------------

uint8_t drumInstrumentForNote(uint8_t note)
{
    switch (note)
    {
        case 36:
            return 10; // Kick

        case 38:
            return 11; // Snare

        case 39:
            return 15; // Clap

        case 42:
            return 12; // Closed HH

        case 46:
            return 13; // Open HH

        case 41:
        case 43:
        case 45:
        case 47:
            return 14; // Tom

        default:

            switch (note % 5)
            {
                case 0: return 10;
                case 1: return 11;
                case 2: return 12;
                case 3: return 14;
                default:return 15;
            }
    }
}

// -------------------------------------------------------
// STOP VOCI DI UNA SORGENTE
// -------------------------------------------------------

void stopVoicesBySource(uint8_t source)
{
    std::lock_guard<std::mutex> lock(voiceMutex);

    for (auto &v : voices)
        if (v.active && v.source == source)
            v.active = false;

    for (auto &sv : sampleVoices)
        if (sv.active && sv.source == source)
            sv.active = false;
}

// -------------------------------------------------------
// STOP TUTTE LE VOCI
// -------------------------------------------------------

void stopAllVoices()
{
    std::lock_guard<std::mutex> lock(voiceMutex);

    for (auto &v : voices)
        v.active = false;

    for (auto &sv : sampleVoices)
        sv.active = false;

    for (auto &st : oneShotLiveStates)
    {
        st.state = OneShotLivePlaybackState::Stopped;
        st.position = 0;
        st.lastPressMs = 0;
    }
}

void startSampleVoice(uint8_t note, uint8_t velocity, uint8_t source, size_t startPosition = 0)
{
    if (note > 127 || !sampleMap[note].valid)
    {
        std::cout << "[SAMPLE] Nessun WAV mappato sulla nota " << static_cast<int>(note) << std::endl;
        return;
    }

    std::lock_guard<std::mutex> lock(voiceMutex);
    SampleVoice* selected = nullptr;

    for (auto& sv : sampleVoices)
    {
        if (!sv.active)
        {
            selected = &sv;
            break;
        }
    }

    if (!selected)
        selected = &sampleVoices[0];

    selected->active = true;
    selected->note = note;
    selected->velocity = velocity;
    selected->source = source;
    selected->sample = &sampleMap[note];

    // Se il sample era già in esecuzione quando è iniziata la registrazione
    // del loop, il PLAY può ripartire esattamente da quel punto del WAV.
    if (!selected->sample->left.empty())
        selected->position = std::min(startPosition, selected->sample->left.size() - 1);
    else
        selected->position = 0;
}

void stopSampleVoice(uint8_t note, uint8_t source)
{
    std::lock_guard<std::mutex> lock(voiceMutex);
    for (auto& sv : sampleVoices)
    {
        if (!sv.active || sv.note != note || sv.source != source || !sv.sample)
            continue;

        // oneshot continua fino alla fine anche quando il tasto viene rilasciato
        if (sv.sample->mode != SampleMode::OneShot)
            sv.active = false;
    }
}

// Ferma forzatamente un sample, anche se e' oneshot.
// Usato dal looper e dallo STOP a doppio click.
void forceStopSampleVoice(uint8_t note, uint8_t source)
{
    std::lock_guard<std::mutex> lock(voiceMutex);
    for (auto& sv : sampleVoices)
    {
        if (sv.active && sv.note == note && sv.source == source)
            sv.active = false;
    }
}

// Gestione LIVE dei sample oneshot dello strumento 18:
// 1 pressione: PLAY
// pressione successiva singola: PAUSA
// pressione successiva: RIPRENDE
// doppia pressione rapida (<=350 ms): STOP + reset a zero
// Ritorna: 1=start/resume, 2=pause, 3=stop, 0=errore/non mappato.
int toggleLiveOneShotSample(uint8_t note, uint8_t velocity, size_t &eventStartPosition)
{
    eventStartPosition = 0;

    if (note > 127 || !sampleMap[note].valid)
    {
        std::cout << "[SAMPLE] Nessun WAV mappato sulla nota " << static_cast<int>(note) << std::endl;
        return 0;
    }

    if (sampleMap[note].mode != SampleMode::OneShot)
    {
        startSampleVoice(note, velocity, LIVE_SOURCE, 0);
        return 1;
    }

    uint64_t now = millisNow();
    std::lock_guard<std::mutex> lock(voiceMutex);
    OneShotLiveState &st = oneShotLiveStates[note];

    bool doublePress =
        (st.lastPressMs != 0 && (now - st.lastPressMs) <= ONESHOT_DOUBLE_PRESS_MS);
    st.lastPressMs = now;

    auto findLiveVoice = [&]() -> SampleVoice*
    {
        for (auto &sv : sampleVoices)
        {
            if (sv.active && sv.source == LIVE_SOURCE && sv.note == note && sv.sample)
                return &sv;
        }
        return nullptr;
    };

    if (doublePress)
    {
        if (SampleVoice* sv = findLiveVoice())
            sv->active = false;

        st.state = OneShotLivePlaybackState::Stopped;
        st.position = 0;
        st.velocity = velocity;

        std::cout << "[SAMPLE] nota " << static_cast<int>(note)
                  << " DOUBLE -> STOP / RESET 0:00" << std::endl;
        return 3;
    }

    if (st.state == OneShotLivePlaybackState::Playing)
    {
        if (SampleVoice* sv = findLiveVoice())
        {
            st.position = sv->position;
            sv->active = false;
        }

        st.state = OneShotLivePlaybackState::Paused;
        std::cout << "[SAMPLE] nota " << static_cast<int>(note)
                  << " PAUSA @ frame " << st.position << std::endl;
        return 2;
    }

    size_t startPos = (st.state == OneShotLivePlaybackState::Paused) ? st.position : 0;
    eventStartPosition = startPos;

    SampleVoice* selected = nullptr;
    for (auto &sv : sampleVoices)
    {
        if (!sv.active)
        {
            selected = &sv;
            break;
        }
    }
    if (!selected)
        selected = &sampleVoices[0];

    selected->active = true;
    selected->note = note;
    selected->velocity = velocity;
    selected->source = LIVE_SOURCE;
    selected->sample = &sampleMap[note];
    selected->position = selected->sample->left.empty()
        ? 0
        : std::min(startPos, selected->sample->left.size() - 1);

    st.state = OneShotLivePlaybackState::Playing;
    st.position = selected->position;
    st.velocity = velocity;

    if (startPos == 0)
        std::cout << "[SAMPLE] nota " << static_cast<int>(note) << " PLAY FROM BEGINNING" << std::endl;
    else
        std::cout << "[SAMPLE] nota " << static_cast<int>(note)
                  << " RESUME @ frame " << startPos << std::endl;

    return 1;
}

// -------------------------------------------------------
// START VOICE
// -------------------------------------------------------

void startVoice(
    uint8_t note,
    uint8_t velocity,
    uint8_t instrument,
    uint8_t source
)
{
    std::lock_guard<std::mutex> lock(voiceMutex);

    if (instrument == 16)
        instrument = drumInstrumentForNote(note);

    // Se stessa nota/stessa sorgente esiste già, retrigger
    for (auto &v : voices)
    {
        if (
            v.active &&
            v.note == note &&
            v.source == source
        )
        {
            v.velocity = velocity;
            v.instrument = instrument;
            v.frequency = noteToFrequency(note);
            v.phase = 0.0;
            v.startMs = millisNow();
            v.releasing = false;

            return;
        }
    }

    Voice *selected = nullptr;

    // cerca voce libera
    for (auto &v : voices)
    {
        if (!v.active)
        {
            selected = &v;
            break;
        }
    }

    // Voice stealing
    if (!selected)
    {
        selected = &voices[0];

        for (auto &v : voices)
        {
            if (v.startMs < selected->startMs)
                selected = &v;
        }
    }

    selected->active = false;

    selected->note = note;
    selected->velocity = velocity;
    selected->instrument = instrument;
    selected->source = source;

    selected->frequency = noteToFrequency(note);
    selected->phase = 0.0;

    selected->startMs = millisNow();

    selected->releasing = false;
    selected->releaseStartMs = 0;

    selected->active = true;
}

// -------------------------------------------------------
// NOTE OFF
// -------------------------------------------------------

void stopVoice(
    uint8_t note,
    uint8_t source
)
{
    std::lock_guard<std::mutex> lock(voiceMutex);

    uint64_t now = millisNow();

    for (auto &v : voices)
    {
        if (
            v.active &&
            v.note == note &&
            v.source == source
        )
        {
            // percussioni = one shot
            if (
                v.instrument >= 10 &&
                v.instrument <= 15
            )
            {
                continue;
            }

            v.releasing = true;
            v.releaseStartMs = now;
        }
    }
}

void startInstrumentNote(
    uint8_t note,
    uint8_t velocity,
    uint8_t instrument,
    uint8_t source,
    size_t sampleStartPosition = 0
)
{
    if (instrument == SAMPLE_INSTRUMENT)
        startSampleVoice(note, velocity, source, sampleStartPosition);
    else
        startVoice(note, velocity, instrument, source);
}

void stopInstrumentNote(uint8_t note, uint8_t instrument, uint8_t source)
{
    if (instrument == SAMPLE_INSTRUMENT)
        stopSampleVoice(note, source);
    else
        stopVoice(note, source);
}

// -------------------------------------------------------
// SINTESI DI UNA VOCE
// -------------------------------------------------------

double generateVoiceSample(
    Voice &v,
    uint64_t now
)
{
    if (!v.active)
        return 0.0;

    double age =
        (now - v.startMs) / 1000.0;

    double velocity =
        v.velocity / 127.0;

    double sample = 0.0;
    double envelope = 1.0;

    double freq = v.frequency;

    switch (v.instrument)
    {
        // ------------------------------------------------
        // 1 SQUARE
        // ------------------------------------------------

        case 1:
            sample =
                v.phase < 0.5
                ? 1.0
                : -1.0;
            break;

        // ------------------------------------------------
        // 2 SAW
        // ------------------------------------------------

        case 2:
            sample =
                (v.phase * 2.0) - 1.0;
            break;

        // ------------------------------------------------
        // 3 TRIANGLE
        // ------------------------------------------------

        case 3:
            sample =
                1.0 -
                4.0 *
                std::fabs(v.phase - 0.5);
            break;

        // ------------------------------------------------
        // 4 SINE
        // ------------------------------------------------

        case 4:
            sample =
                std::sin(
                    2.0 * PI * v.phase
                );
            break;

        // ------------------------------------------------
        // 5 ORGAN
        // ------------------------------------------------

        case 5:
        {
            double p1 =
                std::sin(
                    2.0 * PI * v.phase
                );

            double p2 =
                std::sin(
                    4.0 * PI * v.phase
                ) * 0.50;

            double p3 =
                std::sin(
                    6.0 * PI * v.phase
                ) * 0.25;

            sample =
                (p1 + p2 + p3) / 1.75;

            break;
        }

        // ------------------------------------------------
        // 6 BASS
        // ------------------------------------------------

        case 6:
        {
            double square =
                v.phase < 0.5
                ? 1.0
                : -1.0;

            double sine =
                std::sin(
                    2.0 * PI * v.phase
                );

            sample =
                square * 0.55 +
                sine * 0.45;

            break;
        }

        // ------------------------------------------------
        // 7 LEAD
        // ------------------------------------------------

        case 7:
        {
            double saw =
                v.phase * 2.0 - 1.0;

            double square =
                v.phase < 0.5
                ? 1.0
                : -1.0;

            sample =
                saw * 0.65 +
                square * 0.35;

            break;
        }

        // ------------------------------------------------
        // 8 PLUCK
        // ------------------------------------------------

        case 8:
        {
            sample =
                1.0 -
                4.0 *
                std::fabs(v.phase - 0.5);

            envelope =
                std::exp(-4.0 * age);

            if (age > 2.5)
            {
                v.active = false;
                return 0;
            }

            break;
        }

        // ------------------------------------------------
        // 9 BELL
        // ------------------------------------------------

        case 9:
        {
            double a =
                std::sin(
                    2.0 * PI * v.phase
                );

            double b =
                std::sin(
                    2.0 *
                    PI *
                    v.phase *
                    2.01
                ) * 0.55;

            double c =
                std::sin(
                    2.0 *
                    PI *
                    v.phase *
                    3.97
                ) * 0.30;

            sample =
                (a + b + c) / 1.85;

            envelope =
                std::exp(-2.0 * age);

            if (age > 4.0)
            {
                v.active = false;
                return 0;
            }

            break;
        }

        // ------------------------------------------------
        // 10 KICK
        // ------------------------------------------------

        case 10:
        {
            if (age > 0.50)
            {
                v.active = false;
                return 0;
            }

            freq =
                45.0 +
                120.0 *
                std::exp(-15.0 * age);

            sample =
                std::sin(
                    2.0 * PI * v.phase
                );

            envelope =
                std::exp(-8.0 * age);

            break;
        }

        // ------------------------------------------------
        // 11 SNARE
        // ------------------------------------------------

        case 11:
        {
            if (age > 0.35)
            {
                v.active = false;
                return 0;
            }

            double n = noiseSample();

            double tone =
                std::sin(
                    2.0 * PI * v.phase
                );

            sample =
                n * 0.75 +
                tone * 0.25;

            envelope =
                std::exp(-10.0 * age);

            freq = 180.0;

            break;
        }

        // ------------------------------------------------
        // 12 CLOSED HI-HAT
        // ------------------------------------------------

        case 12:
        {
            if (age > 0.12)
            {
                v.active = false;
                return 0;
            }

            sample = noiseSample();

            envelope =
                std::exp(-30.0 * age);

            break;
        }

        // ------------------------------------------------
        // 13 OPEN HI-HAT
        // ------------------------------------------------

        case 13:
        {
            if (age > 0.75)
            {
                v.active = false;
                return 0;
            }

            sample = noiseSample();

            envelope =
                std::exp(-6.0 * age);

            break;
        }

        // ------------------------------------------------
        // 14 TOM
        // ------------------------------------------------

        case 14:
        {
            if (age > 0.60)
            {
                v.active = false;
                return 0;
            }

            freq =
                95.0 +
                70.0 *
                std::exp(-8.0 * age);

            sample =
                std::sin(
                    2.0 * PI * v.phase
                );

            envelope =
                std::exp(-6.0 * age);

            break;
        }

        // ------------------------------------------------
        // 15 CLAP
        // ------------------------------------------------

        case 15:
        {
            if (age > 0.40)
            {
                v.active = false;
                return 0;
            }

            double burst = 0.0;

            if (
                age < 0.04 ||
                (age > 0.07 && age < 0.11) ||
                (age > 0.14 && age < 0.19)
            )
                burst = 1.0;
            else
                burst = 0.35;

            sample =
                noiseSample() *
                burst;

            envelope =
                std::exp(-7.0 * age);

            break;
        }
    }

    // ---------------------------------------------------
    // RELEASE
    // ---------------------------------------------------

    if (v.releasing)
    {
        double releaseAge =
            (now - v.releaseStartMs) /
            1000.0;

        constexpr double releaseTime =
            0.080;

        if (releaseAge >= releaseTime)
        {
            v.active = false;
            return 0.0;
        }

        envelope *=
            1.0 -
            (releaseAge / releaseTime);
    }

    // ---------------------------------------------------
    // PHASE
    // ---------------------------------------------------

    v.phase +=
        freq /
        SAMPLE_RATE;

    if (v.phase >= 1.0)
        v.phase -=
            std::floor(v.phase);

    double instVolume = 1.0;
    if (v.instrument >= 1 && v.instrument <= 16)
        instVolume = instrumentVolume[v.instrument].load();

    return
        sample *
        envelope *
        velocity *
        instVolume;
}

// -------------------------------------------------------
// APERTURA AUDIO AUTOMATICA
// -------------------------------------------------------

snd_pcm_t* openAudioDevice(
    std::string &selectedName
)
{
    const char* candidates[] =
    {
        "plughw:CARD=Device,DEV=0",      // USB Audio principale
        "plughw:CARD=vc4hdmi0,DEV=0",   // HDMI fallback
        "plughw:CARD=vc4hdmi1,DEV=0",   // HDMI fallback
        "default"
    };

    for (const char* device : candidates)
    {
        std::cout
            << "[AUDIO] Provo "
            << device
            << std::endl;

        snd_pcm_t* pcm = nullptr;

        int err =
            snd_pcm_open(
                &pcm,
                device,
                SND_PCM_STREAM_PLAYBACK,
                0
            );

        if (err < 0)
            continue;

        err =
            snd_pcm_set_params(
                pcm,
                SND_PCM_FORMAT_S16_LE,
                SND_PCM_ACCESS_RW_INTERLEAVED,
                CHANNELS,
                SAMPLE_RATE,
                1,
                10000
            );

        if (err >= 0)
        {
            selectedName = device;

            std::cout
                << "[AUDIO] OK -> "
                << selectedName
                << std::endl;

            return pcm;
        }

        snd_pcm_close(pcm);
    }

    return nullptr;
}

// -------------------------------------------------------
// AUDIO THREAD
// -------------------------------------------------------

void audioThread()
{
    int16_t buffer[
        BUFFER_FRAMES *
        CHANNELS
    ];

    while (running)
    {
        std::string audioName;

        snd_pcm_t* pcm =
            openAudioDevice(
                audioName
            );

        if (!pcm)
        {
            std::cerr
                << "[AUDIO] Nessuna uscita disponibile."
                << std::endl;

            std::this_thread::sleep_for(
                std::chrono::seconds(2)
            );

            continue;
        }

        double tremPhase = 0.0;

        bool deviceOK = true;

        while (running && deviceOK)
        {
            {
                std::lock_guard<std::mutex>
                    lock(voiceMutex);

                for (
                    int frame = 0;
                    frame < BUFFER_FRAMES;
                    frame++
                )
                {
                    uint64_t now =
                        millisNow();

                    // Manteniamo due percorsi separati:
                    // 1) synth 1-16 -> puo' mantenere la quantizzazione retro 8 bit
                    // 2) WAV instrument 18 -> percorso stereo pulito, senza quantizzazione 8 bit
                    double synthL = 0.0;
                    double synthR = 0.0;
                    double wavL = 0.0;
                    double wavR = 0.0;
                    int activeSynthCount = 0;
                    int activeWavCount = 0;

                    for (auto &v : voices)
                    {
                        if (!v.active) continue;
                        double mono = generateVoiceSample(v, now);
                        synthL += mono;
                        synthR += mono;
                        activeSynthCount++;
                    }

                    for (auto &sv : sampleVoices)
                    {
                        if (!sv.active || !sv.sample || !sv.sample->valid) continue;
                        const SampleData& sd = *sv.sample;
                        if (sd.left.empty()) { sv.active = false; continue; }

                        if (sv.position >= sd.left.size())
                        {
                            if (sd.mode == SampleMode::Loop)
                                sv.position = 0;
                            else
                            {
                                if (sd.mode == SampleMode::OneShot && sv.source == LIVE_SOURCE)
                                {
                                    OneShotLiveState &st = oneShotLiveStates[sv.note];
                                    st.state = OneShotLivePlaybackState::Stopped;
                                    st.position = 0;
                                }
                                sv.active = false;
                                continue;
                            }
                        }

                        size_t p = sv.position++;
                        if (sd.mode == SampleMode::OneShot && sv.source == LIVE_SOURCE)
                            oneShotLiveStates[sv.note].position = sv.position;

                        float velocityGain = sv.velocity / 127.0f;
                        float kitGain = instrumentVolume[SAMPLE_INSTRUMENT].load();
                        float gain = velocityGain * sd.volume * kitGain;

                        // WAV pulito: preserva L/R originali e precisione float
                        wavL += static_cast<double>(sd.left[p]) * gain;
                        wavR += static_cast<double>(sd.right[p]) * gain;
                        activeWavCount++;
                    }

                    // Normalizzazione separata: i WAV non vengono attenuati a causa
                    // delle voci synth attive e viceversa.
                    if (activeSynthCount > 1)
                    {
                        double norm = std::sqrt(static_cast<double>(activeSynthCount));
                        synthL /= norm;
                        synthR /= norm;
                    }

                    if (activeWavCount > 1)
                    {
                        double norm = std::sqrt(static_cast<double>(activeWavCount));
                        wavL /= norm;
                        wavR /= norm;
                    }

                    // ----------------------------------
                    // TREMOLO: solo sul motore synth 1-16.
                    // I WAV dello strumento 18 restano fedeli all'originale.
                    // ----------------------------------

                    double depth = tremoloDepth.load();

                    if (depth > 0.001)
                    {
                        double trem =
                            0.5 +
                            0.5 *
                            std::sin(
                                2.0 *
                                PI *
                                tremPhase
                            );

                        double tremGain = (1.0 - depth) + depth * trem;
                        synthL *= tremGain;
                        synthR *= tremGain;

                        tremPhase +=
                            6.0 /
                            SAMPLE_RATE;

                        if (tremPhase >= 1.0)
                            tremPhase -= 1.0;
                    }

                    // Il percorso synth mantiene il carattere retro.
                    synthL *= 0.40;
                    synthR *= 0.40;
                    synthL = std::clamp(synthL, -1.0, 1.0);
                    synthR = std::clamp(synthR, -1.0, 1.0);

                    int synth8L = std::clamp(static_cast<int>((synthL + 1.0) * 127.5), 0, 255);
                    int synth8R = std::clamp(static_cast<int>((synthR + 1.0) * 127.5), 0, 255);
                    double retroSynthL = (synth8L - 128) / 128.0;
                    double retroSynthR = (synth8R - 128) / 128.0;

                    // I WAV entrano nel mix DOPO la quantizzazione retro del synth.
                    // In questo modo non ricevono il fruscio/rumore di quantizzazione 8 bit.
                    double finalL = retroSynthL + wavL;
                    double finalR = retroSynthR + wavR;

                    // Limiter trasparente finale per evitare overflow int16.
                    finalL = std::clamp(finalL, -1.0, 1.0);
                    finalR = std::clamp(finalR, -1.0, 1.0);

                    buffer[frame * 2] = static_cast<int16_t>(std::lrint(finalL * 32767.0));
                    buffer[frame * 2 + 1] = static_cast<int16_t>(std::lrint(finalR * 32767.0));
                }
            }

            // r/s registra sempre il mix finale completo, indipendentemente dallo strumento
            appendRecording(buffer, BUFFER_FRAMES);

            snd_pcm_sframes_t result =
                snd_pcm_writei(
                    pcm,
                    buffer,
                    BUFFER_FRAMES
                );

            if (result < 0)
            {
                result =
                    snd_pcm_recover(
                        pcm,
                        result,
                        1
                    );

                if (result < 0)
                {
                    std::cerr
                        << "[AUDIO] Connessione persa: "
                        << snd_strerror(result)
                        << std::endl;

                    deviceOK = false;
                }
            }
        }

        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);

        if (running)
        {
            std::cout
                << "[AUDIO] Riprovo..."
                << std::endl;

            std::this_thread::sleep_for(
                std::chrono::seconds(1)
            );
        }
    }
}

// -------------------------------------------------------
// REGISTRA EVENTO NEI LOOP IN RECORD
// -------------------------------------------------------

void recordMidiEvent(
    bool noteOn,
    uint8_t note,
    uint8_t velocity,
    uint8_t instrument,
    size_t sampleStartPosition = 0
)
{
    uint64_t now = millisNow();

    for (int t = 0; t < NUM_TRACKS; t++)
    {
        if (!tracks[t].recording)
            continue;

        MidiEvent ev;

        ev.timeMs =
            static_cast<uint32_t>(
                now -
                tracks[t].recordStart
            );

        ev.noteOn = noteOn;
        ev.note = note;
        ev.velocity = velocity;
        ev.instrument = instrument;
        ev.sampleStartPosition = sampleStartPosition;

        tracks[t].events.push_back(
            ev
        );
    }
}

// -------------------------------------------------------
// CATTURA SAMPLE GIÀ ATTIVI ALL'INIZIO DEL REC
// -------------------------------------------------------

void captureActiveSamplesAtRecordStart(int track)
{
    LoopTrack &t = tracks[track];

    std::lock_guard<std::mutex> lock(voiceMutex);

    size_t captured = 0;

    for (const auto &sv : sampleVoices)
    {
        if (!sv.active || !sv.sample || !sv.sample->valid)
            continue;

        // Ci interessano solo i sample suonati dal vivo.
        // Le voci appartenenti ad altre tracce del looper non vanno copiate.
        if (sv.source != LIVE_SOURCE)
            continue;

        MidiEvent ev{};
        ev.timeMs = 0;
        ev.noteOn = true;
        ev.note = sv.note;
        ev.velocity = sv.velocity;
        ev.instrument = SAMPLE_INSTRUMENT;
        ev.sampleStartPosition = sv.position;

        t.events.push_back(ev);
        captured++;

        std::cout
            << "[REC " << track + 1 << "] WAV attivo catturato: nota "
            << static_cast<int>(sv.note)
            << " offset frame " << sv.position
            << std::endl;
    }

    if (captured > 0)
    {
        std::cout
            << "[REC " << track + 1 << "] "
            << captured
            << " sample già in play aggiunti a t=0"
            << std::endl;
    }
}

// -------------------------------------------------------
// RECORD TOGGLE
// -------------------------------------------------------

void toggleRecord(int track)
{
    LoopTrack &t =
        tracks[track];

    if (!t.recording)
    {
        if (t.playing)
        {
            t.playing = false;

            stopVoicesBySource(
                track
            );
        }

        t.events.clear();

        t.recordStart =
            millisNow();

        t.duration = 0;
        t.playhead = 0;
        t.previousPos = 0;
        t.playbackIndex = 0;

        t.recording = true;

        // Se un WAV dello strumento 18 era già partito prima di REC,
        // salviamo a t=0 la posizione corrente. In questo modo il loop
        // registra davvero il pezzo del WAV scelto, invece di risultare vuoto.
        captureActiveSamplesAtRecordStart(track);

        std::cout
            << "[REC "
            << track + 1
            << "] START"
            << std::endl;
    }
    else
    {
        t.recording = false;

        t.duration =
            static_cast<uint32_t>(
                millisNow() -
                t.recordStart
            );

        if (t.duration < 1)
            t.duration = 1;

        std::cout
            << "[REC "
            << track + 1
            << "] STOP - "
            << t.events.size()
            << " eventi - "
            << t.duration
            << " ms"
            << std::endl;
    }
}

// -------------------------------------------------------
// PLAY / PAUSE
// -------------------------------------------------------

void togglePlay(int track)
{
    LoopTrack &t = tracks[track];

    if (t.events.empty())
    {
        std::cout << "[PLAY " << track + 1 << "] traccia vuota" << std::endl;
        return;
    }

    if (t.duration == 0) return;

    if (!t.playing)
    {
        // Ogni nuovo PLAY parte SEMPRE dall'inizio.
        t.playhead = 0;
        t.playStart = millisNow();
        t.playbackIndex = 0;
        t.previousPos = 0;
        stopVoicesBySource(track);
        t.playing = true;
        std::cout << "[PLAY " << track + 1 << "] START FROM BEGINNING" << std::endl;
    }
    else
    {
        // Il secondo click non mette in pausa: ferma e resetta a 0.
        t.playing = false;
        t.playhead = 0;
        t.playbackIndex = 0;
        t.previousPos = 0;
        stopVoicesBySource(track);
        std::cout << "[PLAY " << track + 1 << "] STOP -> RESET 0:00" << std::endl;
    }
}

// -------------------------------------------------------
// UPDATE LOOP

void updateLoops()
{
    uint64_t now =
        millisNow();

    for (int i = 0; i < NUM_TRACKS; i++)
    {
        LoopTrack &t =
            tracks[i];

        if (
            !t.playing ||
            t.duration == 0 ||
            t.events.empty()
        )
            continue;

        uint32_t pos =
            static_cast<uint32_t>(
                (now - t.playStart) %
                t.duration
            );

        // nuovo giro
        if (pos < t.previousPos)
        {
            stopVoicesBySource(
                i
            );

            t.playbackIndex = 0;
        }

        while (
            t.playbackIndex <
                t.events.size() &&
            t.events[
                t.playbackIndex
            ].timeMs <= pos
        )
        {
            const MidiEvent &ev =
                t.events[
                    t.playbackIndex
                ];

            if (ev.noteOn)
            {
                startInstrumentNote(
                    ev.note,
                    ev.velocity,
                    ev.instrument,
                    i,
                    ev.sampleStartPosition
                );
            }
            else
            {
                if (ev.instrument == SAMPLE_INSTRUMENT)
                    forceStopSampleVoice(ev.note, i);
                else
                    stopInstrumentNote(ev.note, ev.instrument, i);
            }

            t.playbackIndex++;
        }

        t.previousPos = pos;
    }
}

// -------------------------------------------------------
// CAMBIO STRUMENTO
// -------------------------------------------------------

void nextInstrument()
{
    if (currentInstrument >= 1 && currentInstrument < 16)
        currentInstrument++;
    else if (currentInstrument == 16)
        currentInstrument = SAMPLE_INSTRUMENT;
    else
        currentInstrument = 1;

    std::cout << "[INSTRUMENT] " << currentInstrument << " - "
              << instrumentNames[currentInstrument] << std::endl;
}

void showInstrumentList()
{
    std::cout << std::endl;
    std::cout << "===== STRUMENTI =====" << std::endl;
    for (int i = 1; i <= 16; i++)
        std::cout << i << " - " << instrumentNames[i] << std::endl;
    std::cout << "18 - " << instrumentNames[18] << std::endl;
    std::cout << "=====================" << std::endl << std::endl;
}

void selectInstrument(int number)
{
    if (!((number >= 1 && number <= 16) || number == SAMPLE_INSTRUMENT))
    {
        std::cout << "[INSTRUMENT] Numero non valido. Usa 1-16 oppure 18." << std::endl;
        return;
    }

    currentInstrument = number;

    std::cout
        << "[INSTRUMENT] "
        << currentInstrument
        << " - "
        << instrumentNames[currentInstrument]
        << std::endl;
}

void setCurrentInstrumentVolume(int percent)
{
    if (percent < 0 || percent > 200)
    {
        std::cout << "[VOLUME] Valore non valido. Usa 0-200." << std::endl;
        return;
    }

    instrumentVolume[currentInstrument].store(percent / 100.0f);

    std::cout
        << "[VOLUME] "
        << currentInstrument
        << " - "
        << instrumentNames[currentInstrument]
        << " = "
        << percent
        << "%"
        << std::endl;
}

int getCurrentInstrumentVolumePercent()
{
    return static_cast<int>(
        std::lround(instrumentVolume[currentInstrument].load() * 100.0f)
    );
}

void showStatus()
{
    std::cout << std::endl;
    std::cout << "=== SYNTH STATUS ===" << std::endl;
    std::cout << "Instrument: " << currentInstrument << " - " << instrumentNames[currentInstrument] << std::endl;
    std::cout << "Volume strumento: " << getCurrentInstrumentVolumePercent() << "%" << std::endl;
    std::cout << "Tremolo: " << static_cast<int>(tremoloDepth.load() * 127.0f) << "/127" << std::endl;
    {
        std::lock_guard<std::mutex> lock(recorderMutex);
        std::cout << "Audio recording: " << (audioRecording ? "ON" : "OFF");
        if (audioRecording) std::cout << " -> " << currentRecordingPath;
        std::cout << std::endl;
    }
    int mapped = 0;
    for (const auto& smp : sampleMap) if (smp.valid) mapped++;
    std::cout << "WAV samples mapped: " << mapped << std::endl;

    for (int i = 0; i < NUM_TRACKS; i++)
    {
        std::cout << "Loop " << i + 1 << ": ";
        if (tracks[i].recording) std::cout << "REC";
        else if (tracks[i].playing) std::cout << "PLAY";
        else if (!tracks[i].events.empty()) std::cout << "STOPPED";
        else std::cout << "EMPTY";
        std::cout << " | events=" << tracks[i].events.size() << std::endl;
    }

    std::cout << "====================" << std::endl;
}

void handleTerminalCommand(const std::string &command)
{
    if (command.empty()) return;

    if (command == "i") { showInstrumentList(); return; }
    if (command == "n") { nextInstrument(); return; }
    if (command == "status") { showStatus(); return; }

    if (command == "vol")
    {
        std::cout
            << "[VOLUME] "
            << currentInstrument
            << " - "
            << instrumentNames[currentInstrument]
            << " = "
            << getCurrentInstrumentVolumePercent()
            << "%"
            << std::endl;
        return;
    }

    if (command.rfind("v ", 0) == 0 || command.rfind("vol ", 0) == 0)
    {
        try
        {
            size_t pos = command.find(' ');
            int percent = std::stoi(command.substr(pos + 1));
            setCurrentInstrumentVolume(percent);
        }
        catch (...)
        {
            std::cout << "[VOLUME] Usa: v 0-200" << std::endl;
        }
        return;
    }

    if (command == "r")
    {
        startAudioRecording();
        return;
    }

    if (command == "s")
    {
        stopAudioRecording();
        return;
    }

    if (command == "recstatus")
    {
        std::lock_guard<std::mutex> lock(recorderMutex);
        std::cout << "[RECORD] " << (audioRecording ? "ON" : "OFF");
        if (audioRecording) std::cout << " -> " << currentRecordingPath;
        std::cout << std::endl;
        return;
    }

    if (command == "stop")
    {
        stopAllVoices();
        std::cout << "[SYNTH] Tutte le note fermate." << std::endl;
        return;
    }

    try
    {
        int number = std::stoi(command);
        selectInstrument(number);
    }
    catch (...)
    {
        std::cout << "[COMMAND] Usa 1-16/18, i, n, v 0-200, vol, r, s, recstatus, status oppure stop." << std::endl;
    }
}

void checkTerminalInput()
{
    pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int result = poll(&pfd, 1, 0);

    if (result > 0 && (pfd.revents & POLLIN))
    {
        std::string line;
        if (std::getline(std::cin, line))
        {
            handleTerminalCommand(line);
        }
    }
}

// -------------------------------------------------------
// MIDI NOTE ON
// -------------------------------------------------------

void handleNoteOn(
    uint8_t channel,
    uint8_t note,
    uint8_t velocity
)
{
    // ---------------------------------------------------
    // CH1 = TASTIERA
    // ---------------------------------------------------

    if (channel == 0)
    {
        std::cout
            << "NOTE ON  "
            << static_cast<int>(note)
            << " vel "
            << static_cast<int>(velocity)
            << " | "
            << instrumentNames[
                currentInstrument
            ]
            << std::endl;

        uint8_t instrumentForThisNote = static_cast<uint8_t>(currentInstrument);
        liveNoteInstrument[note] = instrumentForThisNote;

        if (instrumentForThisNote == SAMPLE_INSTRUMENT &&
            note <= 127 && sampleMap[note].valid &&
            sampleMap[note].mode == SampleMode::OneShot)
        {
            size_t eventStartPosition = 0;
            int action = toggleLiveOneShotSample(note, velocity, eventStartPosition);

            // Nel looper salviamo le transizioni udibili:
            // PLAY/RESUME = NOTE ON con offset corrente
            // PAUSA/STOP = NOTE OFF forzato in playback
            if (action == 1)
                recordMidiEvent(true, note, velocity, instrumentForThisNote, eventStartPosition);
            else if (action == 2 || action == 3)
                recordMidiEvent(false, note, 0, instrumentForThisNote, 0);

            return;
        }

        startInstrumentNote(
            note,
            velocity,
            instrumentForThisNote,
            LIVE_SOURCE
        );

        recordMidiEvent(
            true,
            note,
            velocity,
            instrumentForThisNote
        );

        return;
    }

    // ---------------------------------------------------
    // CH10 = CONTROLLI
    // ---------------------------------------------------

    if (channel == 9)
    {
        for (
            int i = 0;
            i < NUM_TRACKS;
            i++
        )
        {
            if (
                note ==
                REC_NOTES[i]
            )
            {
                toggleRecord(i);
                return;
            }

            if (
                note ==
                PLAY_NOTES[i]
            )
            {
                togglePlay(i);
                return;
            }
        }

        if (
            note ==
            NEXT_INSTRUMENT_NOTE
        )
        {
            nextInstrument();
            return;
        }
    }
}

// -------------------------------------------------------
// MIDI NOTE OFF
// -------------------------------------------------------

void handleNoteOff(
    uint8_t channel,
    uint8_t note
)
{
    if (channel != 0)
        return;

    uint8_t instrumentForThisNote = liveNoteInstrument[note];
    if (instrumentForThisNote == 0)
        instrumentForThisNote = static_cast<uint8_t>(currentInstrument);

    // Il NOTE OFF fisico di un oneshot LIVE non cambia stato:
    // il controllo PLAY/PAUSA/RESUME/STOP avviene sui NOTE ON successivi.
    if (instrumentForThisNote == SAMPLE_INSTRUMENT &&
        note <= 127 && sampleMap[note].valid &&
        sampleMap[note].mode == SampleMode::OneShot)
    {
        liveNoteInstrument[note] = 0;
        return;
    }

    stopInstrumentNote(
        note,
        instrumentForThisNote,
        LIVE_SOURCE
    );

    recordMidiEvent(
        false,
        note,
        0,
        instrumentForThisNote
    );

    liveNoteInstrument[note] = 0;
}

// -------------------------------------------------------
// MIDI CC
// -------------------------------------------------------

void handleCC(
    uint8_t channel,
    uint8_t controller,
    uint8_t value
)
{
    if (
        channel == 0 &&
        controller == 30
    )
    {
        float depth =
            value /
            127.0f;

        tremoloDepth.store(
            depth
        );

        std::cout
            << "[TREMOLO] "
            << static_cast<int>(
                value
            )
            << " / 127"
            << std::endl;
    }
}

// -------------------------------------------------------
// CERCA SMK25II
// -------------------------------------------------------

bool findMidiSource(
    snd_seq_t* seq,
    int &clientOut,
    int &portOut
)
{
    snd_seq_client_info_t* cinfo;

    snd_seq_client_info_alloca(
        &cinfo
    );

    snd_seq_client_info_set_client(
        cinfo,
        -1
    );

    while (
        snd_seq_query_next_client(
            seq,
            cinfo
        ) >= 0
    )
    {
        int client =
            snd_seq_client_info_get_client(
                cinfo
            );

        const char* name =
            snd_seq_client_info_get_name(
                cinfo
            );

        if (!name)
            continue;

        std::string n(name);

        if (
            n.find("SMK25II") ==
            std::string::npos
        )
            continue;

        snd_seq_port_info_t* pinfo;

        snd_seq_port_info_alloca(
            &pinfo
        );

        snd_seq_port_info_set_client(
            pinfo,
            client
        );

        snd_seq_port_info_set_port(
            pinfo,
            -1
        );

        while (
            snd_seq_query_next_port(
                seq,
                pinfo
            ) >= 0
        )
        {
            unsigned int caps =
                snd_seq_port_info_get_capability(
                    pinfo
                );

            if (
                (caps &
                    SND_SEQ_PORT_CAP_READ) &&
                (caps &
                    SND_SEQ_PORT_CAP_SUBS_READ)
            )
            {
                clientOut = client;

                portOut =
                    snd_seq_port_info_get_port(
                        pinfo
                    );

                return true;
            }
        }
    }

    return false;
}

// -------------------------------------------------------
// CONTROLLA CHE MIDI ESISTA ANCORA
// -------------------------------------------------------

bool midiPortExists(
    snd_seq_t* seq,
    int client,
    int port
)
{
    snd_seq_port_info_t* pinfo;

    snd_seq_port_info_alloca(
        &pinfo
    );

    return
        snd_seq_get_any_port_info(
            seq,
            client,
            port,
            pinfo
        ) >= 0;
}

// -------------------------------------------------------
// CTRL+C
// -------------------------------------------------------

void signalHandler(int)
{
    running = false;
}

// -------------------------------------------------------
// MAIN
// -------------------------------------------------------

int main()
{
    std::signal(
        SIGINT,
        signalHandler
    );

    std::signal(
        SIGTERM,
        signalHandler
    );

    std::cout << std::endl;

    std::cout
        << "=========================================="
        << std::endl;

    std::cout
        << " M-VAVE Raspberry Pi Retro Synth / Looper "
        << std::endl;

    std::cout
        << "=========================================="
        << std::endl;

    std::cout
        << "32 voice polyphony"
        << std::endl;

    std::cout
        << "16 synth instruments + WAV Sample Kit (18)"
        << std::endl;

    std::cout
        << "7 MIDI loop tracks"
        << std::endl;

    std::cout
        << "Automatic MIDI + HDMI detection"
        << std::endl;

    std::cout
        << "USB Audio preferred + HDMI fallback\nClean stereo WAV path for instrument 18"
        << std::endl;

    std::cout
        << "Terminal commands:"
        << std::endl;

    std::cout << "1-16   Select synth instrument" << std::endl;
    std::cout << "18     WAV Sample Kit (samples/map.txt; oneshot: press=pause/resume, double=stop)" << std::endl;
    std::cout << "i      Instrument list" << std::endl;
    std::cout << "n      Next instrument" << std::endl;
    std::cout << "v N    Volume strumento corrente (0-200)" << std::endl;
    std::cout << "vol    Mostra volume strumento corrente" << std::endl;
    std::cout << "status Synth status" << std::endl;
    std::cout << "r      Start recording del mix finale" << std::endl;
    std::cout << "s      Stop + salva WAV" << std::endl;
    std::cout << "recstatus Stato registrazione audio" << std::endl;
    std::cout << "stop   Stop all notes" << std::endl;
    std::cout << std::endl;

    for (int i = 0; i <= 18; i++)
        instrumentVolume[i].store(1.0f);

    loadSampleMap("samples/map.txt");
    showInstrumentList();

    for (
        int i = 0;
        i < NUM_TRACKS;
        i++
    )
    {
        tracks[i].events.reserve(
            4096
        );
    }

    // ---------------------------------------------------
    // ALSA MIDI
    // ---------------------------------------------------

    snd_seq_t* seq = nullptr;

    int err =
        snd_seq_open(
            &seq,
            "default",
            SND_SEQ_OPEN_INPUT,
            SND_SEQ_NONBLOCK
        );

    if (err < 0)
    {
        std::cerr
            << "Errore ALSA MIDI: "
            << snd_strerror(err)
            << std::endl;

        return 1;
    }

    snd_seq_set_client_name(
        seq,
        "MVAVE Retro Synth"
    );

    int localPort =
        snd_seq_create_simple_port(
            seq,
            "MIDI Input",
            SND_SEQ_PORT_CAP_WRITE |
            SND_SEQ_PORT_CAP_SUBS_WRITE,
            SND_SEQ_PORT_TYPE_APPLICATION
        );

    if (localPort < 0)
    {
        std::cerr
            << "Errore creazione porta MIDI."
            << std::endl;

        return 1;
    }

    // ---------------------------------------------------
    // AUDIO
    // ---------------------------------------------------

    std::thread audio(
        audioThread
    );

    // ---------------------------------------------------
    // MIDI RECONNECT
    // ---------------------------------------------------

    int midiClient = -1;
    int midiPort = -1;

    bool midiConnected = false;

    uint64_t lastMidiSearch = 0;
    uint64_t lastMidiCheck = 0;

    while (running)
    {
        uint64_t now =
            millisNow();

        // -----------------------------------------------
        // CERCA TASTIERA
        // -----------------------------------------------

        if (!midiConnected)
        {
            if (
                now -
                lastMidiSearch >=
                1000
            )
            {
                lastMidiSearch = now;

                int client;
                int port;

                if (
                    findMidiSource(
                        seq,
                        client,
                        port
                    )
                )
                {
                    std::cout
                        << "[MIDI] SMK25II trovata: "
                        << client
                        << ":"
                        << port
                        << std::endl;

                    int result =
                        snd_seq_connect_from(
                            seq,
                            localPort,
                            client,
                            port
                        );

                    if (
                        result >= 0 ||
                        result == -EBUSY
                    )
                    {
                        midiClient =
                            client;

                        midiPort =
                            port;

                        midiConnected =
                            true;

                        std::cout
                            << "[MIDI] CONNESSA"
                            << std::endl;
                    }
                    else
                    {
                        std::cerr
                            << "[MIDI] Connessione fallita: "
                            << snd_strerror(
                                result
                            )
                            << std::endl;
                    }
                }
                else
                {
                    std::cout
                        << "[MIDI] Attendo SMK25II..."
                        << std::endl;
                }
            }
        }

        // -----------------------------------------------
        // CONTROLLO CONNESSIONE
        // -----------------------------------------------

        if (
            midiConnected &&
            now -
            lastMidiCheck >=
            2000
        )
        {
            lastMidiCheck = now;

            if (
                !midiPortExists(
                    seq,
                    midiClient,
                    midiPort
                )
            )
            {
                std::cout
                    << "[MIDI] SMK25II disconnessa."
                    << std::endl;

                midiConnected =
                    false;

                midiClient = -1;
                midiPort = -1;

                stopAllVoices();
            }
        }

        // -----------------------------------------------
        // LEGGI MIDI
        // -----------------------------------------------

        if (midiConnected)
        {
            snd_seq_event_t* ev =
                nullptr;

            int result;

            while (
                (
                    result =
                    snd_seq_event_input(
                        seq,
                        &ev
                    )
                ) >= 0
            )
            {
                if (!ev)
                    continue;

                switch (ev->type)
                {
                    case SND_SEQ_EVENT_NOTEON:
                    {
                        uint8_t ch =
                            ev->data.note.channel;

                        uint8_t note =
                            ev->data.note.note;

                        uint8_t vel =
                            ev->data.note.velocity;

                        if (vel == 0)
                        {
                            handleNoteOff(
                                ch,
                                note
                            );
                        }
                        else
                        {
                            handleNoteOn(
                                ch,
                                note,
                                vel
                            );
                        }

                        break;
                    }

                    case SND_SEQ_EVENT_NOTEOFF:
                    {
                        handleNoteOff(
                            ev->data.note.channel,
                            ev->data.note.note
                        );

                        break;
                    }

                    case SND_SEQ_EVENT_CONTROLLER:
                    {
                        handleCC(
                            ev->data.control.channel,
                            ev->data.control.param,
                            ev->data.control.value
                        );

                        break;
                    }
                }
            }
        }

        updateLoops();

        checkTerminalInput();

        std::this_thread::sleep_for(
            std::chrono::milliseconds(1)
        );
    }

    // ---------------------------------------------------
    // USCITA
    // ---------------------------------------------------

    stopAllVoices();
    stopAudioRecording();

    running = false;

    if (audio.joinable())
        audio.join();

    snd_seq_close(seq);

    std::cout
        << "Synth terminato."
        << std::endl;

    return 0;
}

