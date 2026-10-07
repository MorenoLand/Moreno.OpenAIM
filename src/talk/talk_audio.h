#pragma once
#include <QByteArray>
#include <QList>
#include <QObject>
#include <memory>

// Sound card I/O of JGTkAol (Research/talk_protocol.md 6.1): winmm, WAVE_MAPPER, 8 kHz 16-bit mono PCM,
// 8 capture buffers of 2880 bytes (180 ms), playback queue of decoded chunks.
class TalkAudio final : public QObject {
  Q_OBJECT
public:
  static constexpr int SampleRate = 8000, ChunkSamples = 1440, ChunkBytes = ChunkSamples * 2;
  explicit TalkAudio(QObject *parent = nullptr);
  ~TalkAudio() override;
  bool open();                       // opens capture and playback (full duplex if both open at once)
  void close();
  bool fullDuplexCapable() const { return duplex_; }
  void setCapturing(bool on);        // captured chunks are delivered only while capturing
  void setPaused(bool paused);       // Pause: neither capture nor playback
  void play(const QByteArray &pcm);  // 16-bit samples
  void setMicGain(int percent);      // 0..100 ("Volume" slider under the own meter)
  void setSpeakerVolume(int percent);
  int micLevel() const { return micLevel_; }      // 0..100 of the last chunk
  int speakerLevel() const { return speakerLevel_; }
signals:
  void captured(const QByteArray &pcm);           // ChunkBytes of 16-bit samples
public:
  struct Impl;
private:
  std::unique_ptr<Impl> d;
  bool duplex_ = false, capturing_ = false, paused_ = false;
  int micGain_ = 100, micLevel_ = 0, speakerLevel_ = 0;
  void deliver(QByteArray pcm);
  void recycleOutput();
};
