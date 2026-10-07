#include "talk_audio.h"
#include <QMetaObject>
#include <QPointer>
#include <cmath>
#include <deque>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

namespace {
int levelOf(const QByteArray &pcm) {
  const auto *s = reinterpret_cast<const qint16 *>(pcm.constData()); const int n = int(pcm.size() / 2); if (n == 0) return 0;
  double sum = 0; for (int i = 0; i < n; ++i) sum += double(s[i]) * s[i];
  const double rms = std::sqrt(sum / n); if (rms < 1) return 0;
  return qBound(0, int((20.0 * std::log10(rms / 32768.0) + 60.0) * 100.0 / 60.0), 100); // -60..0 dBFS -> 0..100
}
}

#ifdef Q_OS_WIN
struct TalkAudio::Impl {
  HWAVEIN in = nullptr; HWAVEOUT out = nullptr;
  WAVEHDR inHeaders[8]{}; QByteArray inBuffers[8];
  std::deque<std::pair<WAVEHDR *, QByteArray *>> playing;
  QPointer<TalkAudio> owner;
  static void CALLBACK inProc(HWAVEIN, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR) {
    if (message != WIM_DATA) return;
    auto *self = reinterpret_cast<Impl *>(instance); auto *header = reinterpret_cast<WAVEHDR *>(param1);
    QByteArray pcm(header->lpData, int(header->dwBytesRecorded));
    // winmm forbids wave calls inside the callback: hand the chunk and the buffer back to the Qt thread.
    QMetaObject::invokeMethod(self->owner.data(), [self, header, pcm] { if (self->owner) self->owner->deliver(pcm); if (self->in) waveInAddBuffer(self->in, header, sizeof(WAVEHDR)); }, Qt::QueuedConnection);
  }
  static void CALLBACK outProc(HWAVEOUT, UINT message, DWORD_PTR instance, DWORD_PTR, DWORD_PTR) {
    if (message != WOM_DONE) return;
    auto *self = reinterpret_cast<Impl *>(instance);
    QMetaObject::invokeMethod(self->owner.data(), [self] { if (self->owner) self->owner->recycleOutput(); }, Qt::QueuedConnection);
  }
};
#else
struct TalkAudio::Impl {};
#endif

TalkAudio::TalkAudio(QObject *parent) : QObject(parent), d(std::make_unique<Impl>()) {}
TalkAudio::~TalkAudio() { close(); }

bool TalkAudio::open() {
#ifdef Q_OS_WIN
  d->owner = this;
  WAVEFORMATEX format{WAVE_FORMAT_PCM, 1, SampleRate, SampleRate * 2, 2, 16, 0};
  const bool output = waveOutOpen(&d->out, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(&Impl::outProc), reinterpret_cast<DWORD_PTR>(d.get()), CALLBACK_FUNCTION) == MMSYSERR_NOERROR;
  const bool input = waveInOpen(&d->in, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(&Impl::inProc), reinterpret_cast<DWORD_PTR>(d.get()), CALLBACK_FUNCTION) == MMSYSERR_NOERROR;
  if (!output) d->out = nullptr; if (!input) d->in = nullptr;
  duplex_ = input && output; // Talk_PrepareAudioLines: full duplex when both devices open together
  if (d->in) {
    for (int i = 0; i < 8; ++i) {
      d->inBuffers[i] = QByteArray(ChunkBytes, 0); d->inHeaders[i] = WAVEHDR{}; d->inHeaders[i].lpData = d->inBuffers[i].data(); d->inHeaders[i].dwBufferLength = ChunkBytes;
      waveInPrepareHeader(d->in, &d->inHeaders[i], sizeof(WAVEHDR)); waveInAddBuffer(d->in, &d->inHeaders[i], sizeof(WAVEHDR));
    }
    waveInStart(d->in);
  }
  return output || input;
#else
  return false;
#endif
}
void TalkAudio::close() {
#ifdef Q_OS_WIN
  if (d->in) { HWAVEIN in = d->in; d->in = nullptr; waveInStop(in); waveInReset(in); for (auto &h : d->inHeaders) waveInUnprepareHeader(in, &h, sizeof(WAVEHDR)); waveInClose(in); }
  if (d->out) { HWAVEOUT out = d->out; d->out = nullptr; waveOutReset(out); for (auto &[h, b] : d->playing) { waveOutUnprepareHeader(out, h, sizeof(WAVEHDR)); delete h; delete b; } d->playing.clear(); waveOutClose(out); }
  d->owner.clear();
#endif
}
void TalkAudio::setCapturing(bool on) { capturing_ = on; if (!on) micLevel_ = 0; }
void TalkAudio::setPaused(bool paused) {
  paused_ = paused;
#ifdef Q_OS_WIN
  if (d->out) { if (paused) waveOutPause(d->out); else waveOutRestart(d->out); }
#endif
}
void TalkAudio::setMicGain(int percent) { micGain_ = qBound(0, percent, 100); }
void TalkAudio::setSpeakerVolume(int percent) {
#ifdef Q_OS_WIN
  const DWORD v = DWORD(qBound(0, percent, 100) * 0xFFFF / 100); if (d->out) waveOutSetVolume(d->out, v | (v << 16));
#else
  Q_UNUSED(percent);
#endif
}
void TalkAudio::deliver(QByteArray pcm) {
  if (!capturing_ || paused_) return;
  if (micGain_ != 100) { auto *s = reinterpret_cast<qint16 *>(pcm.data()); for (int i = 0; i < pcm.size() / 2; ++i) s[i] = qint16(qBound(-32768, s[i] * micGain_ / 100, 32767)); }
  micLevel_ = levelOf(pcm);
  emit captured(pcm);
}
void TalkAudio::play(const QByteArray &pcm) {
#ifdef Q_OS_WIN
  if (!d->out || paused_ || pcm.isEmpty()) return;
  if (d->playing.size() > 8) return; // more than ~1.4 s queued: drop rather than build up delay
  speakerLevel_ = levelOf(pcm);
  auto *buffer = new QByteArray(pcm); auto *header = new WAVEHDR{}; header->lpData = buffer->data(); header->dwBufferLength = DWORD(buffer->size());
  waveOutPrepareHeader(d->out, header, sizeof(WAVEHDR)); waveOutWrite(d->out, header, sizeof(WAVEHDR));
  d->playing.emplace_back(header, buffer);
#else
  Q_UNUSED(pcm);
#endif
}
void TalkAudio::recycleOutput() {
#ifdef Q_OS_WIN
  while (!d->playing.empty() && (d->playing.front().first->dwFlags & WHDR_DONE)) {
    auto [header, buffer] = d->playing.front(); d->playing.pop_front();
    if (d->out) waveOutUnprepareHeader(d->out, header, sizeof(WAVEHDR)); delete header; delete buffer;
  }
  if (d->playing.empty()) speakerLevel_ = 0;
#endif
}
