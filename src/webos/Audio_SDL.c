#include "../Core.h"

#if CC_AUD_BACKEND == CC_AUD_BACKEND_SDL
#include <SDL2/SDL.h>
#include "../Audio.h"

/* SDL2 push-mode audio backend (SDL_OpenAudioDevice + SDL_QueueAudio).
   One audio device is shared by all contexts. Each context converts its
   (channels, S16, sampleRate) source data to the device format with its
   own SDL_AudioStream, then a software mixer sums every active context's
   output into one buffer before it is queued to the device.

   The mixer is kept topped up to MIX_LEAD_FRAMES of device frames using
   SDL_GetQueuedAudioSize (see sm64ex-lgtv's proven SDL push-mode audio).
   It runs from Audio_QueueChunk / Audio_Poll, which the music thread calls
   every ~10ms, so the device never runs dry while audio is active.

   Per-context busy tracking: SDL has no way to ask how much of *this*
   context's data is still queued, so each context counts the source
   samples it has queued (pendingSamples) and consumes them against a
   single startTick using wall-clock time. Volume is applied per context
   when samples are mixed, so changes take effect immediately. */

#define MAX_AUDIO_CONTEXTS 16
#define MIX_MAX_FRAMES  1024
#define MIX_LEAD_FRAMES 2048

struct AudioContext {
	SDL_AudioStream* stream;   /* converts source format -> device format */
	int count, volume;         /* allocated buffers, 0-100 volume */
	int channels, sampleRate;  /* source format */
	cc_uint32 pendingSamples;  /* source samples queued, not yet consumed */
	cc_uint32 bufSamples;      /* samples of the last enqueued buffer */
	cc_uint32 startTick;       /* tick when pendingSamples began playing */
	cc_uint32 pauseTick;       /* tick when playback was paused */
	cc_bool   paused;
	cc_bool   started;
};

#include "../_AudioBase.h"

static SDL_AudioDeviceID sdlDev;
static SDL_AudioSpec sdlHave;
static SDL_mutex* audioMutex;
static int deviceFreq, deviceChannels;

static struct AudioContext* audio_ctxs[MAX_AUDIO_CONTEXTS];
static int audio_ctx_count;
static cc_int16 mixBuffer[MIX_MAX_FRAMES * 2];
static cc_int16 tmpBuffer[MIX_MAX_FRAMES * 2];

static void Mix_Add(cc_int16* dst, const cc_int16* src, int bytes, int volume) {
	int i, v;
	for (i = 0; i < bytes / 2; i++) {
		v = dst[i] + (src[i] * volume / 100);
		if (v > 32767) v = 32767;
		else if (v < -32768) v = -32768;
		dst[i] = (cc_int16)v;
	}
}

static void Mixer_Pump(void) {
	Uint32 queuedFrames, want, bytesPerFrame;
	int i, gotBytes;

	if (!sdlDev) return;
	bytesPerFrame = 2 * deviceChannels;

	queuedFrames = SDL_GetQueuedAudioSize(sdlDev) / bytesPerFrame;
	if (queuedFrames >= MIX_LEAD_FRAMES) return;
	want = MIX_LEAD_FRAMES - queuedFrames;
	if (want > MIX_MAX_FRAMES) want = MIX_MAX_FRAMES;

	if (audioMutex) SDL_LockMutex(audioMutex);
	Mem_Set(mixBuffer, 0, want * bytesPerFrame);
	for (i = 0; i < audio_ctx_count; i++) {
		struct AudioContext* ctx = audio_ctxs[i];
		if (!ctx->stream || ctx->paused || !ctx->started) continue;
		gotBytes = SDL_AudioStreamGet(ctx->stream, tmpBuffer, (int)(want * bytesPerFrame));
		if (gotBytes <= 0) continue;
		Mix_Add(mixBuffer, tmpBuffer, gotBytes, ctx->volume);
	}
	SDL_QueueAudio(sdlDev, mixBuffer, want * bytesPerFrame);
	if (audioMutex) SDL_UnlockMutex(audioMutex);
}

static void RegisterCtx(struct AudioContext* ctx) {
	int i;
	if (audioMutex) SDL_LockMutex(audioMutex);
	for (i = 0; i < audio_ctx_count; i++) {
		if (audio_ctxs[i] == ctx) goto done;
	}
	if (audio_ctx_count < MAX_AUDIO_CONTEXTS) audio_ctxs[audio_ctx_count++] = ctx;
done:
	if (audioMutex) SDL_UnlockMutex(audioMutex);
}

static void UnregisterCtx(struct AudioContext* ctx) {
	int i;
	if (audioMutex) SDL_LockMutex(audioMutex);
	for (i = 0; i < audio_ctx_count; i++) {
		if (audio_ctxs[i] != ctx) continue;
		audio_ctxs[i] = audio_ctxs[--audio_ctx_count];
		break;
	}
	if (audioMutex) SDL_UnlockMutex(audioMutex);
}

cc_bool AudioBackend_Init(void) {
	static const cc_string msg = String_FromConst("Failed to init SDL audio. No audio will play.");
	SDL_AudioSpec want;

	/* Escape hatch for headless/debug runs where webOS audio can be flaky */
	if (getenv("CC_NOSOUND")) return false;
	if (sdlDev) return true;

	SDL_InitSubSystem(SDL_INIT_AUDIO);
	Mem_Set(&want, 0, sizeof(want));
	/* freq/format/channels == 0 make SDL pick the device's native format */
	want.freq     = 0;
	want.format   = 0;
	want.channels = 0;
	want.samples  = 1024;
	want.callback = NULL;

	sdlDev = SDL_OpenAudioDevice(NULL, 0, &want, &sdlHave, 0);
	if (!sdlDev) { Logger_WarnFunc(&msg); SDL_QuitSubSystem(SDL_INIT_AUDIO); return false; }

	/* the software mixer assumes a S16 interleaved device, stereo or mono */
	if (sdlHave.format != AUDIO_S16SYS || sdlHave.channels < 1 || sdlHave.channels > 2) {
		SDL_CloseAudioDevice(sdlDev); sdlDev = 0;
		Logger_WarnFunc(&msg); SDL_QuitSubSystem(SDL_INIT_AUDIO); return false;
	}

	deviceFreq     = sdlHave.freq;
	deviceChannels = sdlHave.channels;
	audioMutex     = SDL_CreateMutex();

	{
		const char* name = SDL_GetCurrentAudioDriver();
		cc_string driver;
		char driverBuf[64];
		String_InitArray(driver, driverBuf);
		if (name) String_AppendConst(&driver, name);
		Platform_Log1("SDL audio driver: %s", &driver);
	}
	SDL_PauseAudioDevice(sdlDev, 0);
	return true;
}

void AudioBackend_Tick(void) { }

void AudioBackend_Free(void) {
	if (audioMutex) { SDL_DestroyMutex(audioMutex); audioMutex = NULL; }
	if (sdlDev) { SDL_CloseAudioDevice(sdlDev); sdlDev = 0; }
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

cc_result Audio_Init(struct AudioContext* ctx, int buffers) {
	ctx->count     = buffers;
	ctx->volume    = 100;
	ctx->paused    = false;
	ctx->started   = false;
	ctx->pendingSamples = 0;
	RegisterCtx(ctx);
	return 0;
}

static void Audio_ResetStream(struct AudioContext* ctx) {
	if (audioMutex) SDL_LockMutex(audioMutex);
	if (ctx->stream) SDL_AudioStreamClear(ctx->stream);
	if (audioMutex) SDL_UnlockMutex(audioMutex);
	ctx->pendingSamples = 0;
	ctx->bufSamples     = 0;
	ctx->startTick      = 0;
	ctx->pauseTick      = 0;
	ctx->paused         = false;
	ctx->started        = false;
}

void Audio_Close(struct AudioContext* ctx) {
	if (audioMutex) SDL_LockMutex(audioMutex);
	if (ctx->stream) { SDL_FreeAudioStream(ctx->stream); ctx->stream = NULL; }
	if (audioMutex) SDL_UnlockMutex(audioMutex);
	Audio_ResetStream(ctx);

	ctx->count      = 0;
	ctx->channels   = 0;
	ctx->sampleRate = 0;
	ctx->volume     = 100;
	UnregisterCtx(ctx);
}

static cc_bool CreateStream(struct AudioContext* ctx, int channels, int sampleRate) {
	if (ctx->stream && ctx->channels == channels && ctx->sampleRate == sampleRate) return true;

	if (audioMutex) SDL_LockMutex(audioMutex);
	if (ctx->stream) { SDL_FreeAudioStream(ctx->stream); ctx->stream = NULL; }

	ctx->channels   = channels;
	ctx->sampleRate = sampleRate;
	ctx->stream = SDL_NewAudioStream(AUDIO_S16SYS, channels, sampleRate, sdlHave.format, sdlHave.channels, sdlHave.freq);
	if (audioMutex) SDL_UnlockMutex(audioMutex);
	return ctx->stream != NULL;
}

static cc_result Audio_SetFormat(struct AudioContext* ctx, int channels, int sampleRate, int playbackRate) {
	sampleRate = Audio_AdjustSampleRate(sampleRate, playbackRate);
	if (!CreateStream(ctx, channels, sampleRate)) return ERR_NOT_SUPPORTED;
	return 0;
}

void Audio_SetVolume(struct AudioContext* ctx, int volume) {
	ctx->volume = volume;
}

static cc_result Audio_QueueChunk(struct AudioContext* ctx, struct AudioChunk* chunk) {
	int res;
	if (audioMutex) SDL_LockMutex(audioMutex);
	res = SDL_AudioStreamPut(ctx->stream, chunk->data, chunk->size);
	if (audioMutex) SDL_UnlockMutex(audioMutex);
	if (res != 0) return ERR_NOT_SUPPORTED;

	ctx->pendingSamples += chunk->size / (2 * ctx->channels);
	ctx->bufSamples      = chunk->size / (2 * ctx->channels);
	/* start the timeline at the first queued sample */
	if (!ctx->started) {
		ctx->started   = true;
		ctx->startTick = SDL_GetTicks();
	}
	Mixer_Pump();
	return 0;
}

static cc_result Audio_Play(struct AudioContext* ctx) {
	cc_uint32 now = SDL_GetTicks();
	if (!ctx->started) {
		ctx->started   = true;
		ctx->startTick = now;
	} else if (ctx->pauseTick) {
		/* skip the time spent paused */
		ctx->startTick += now - ctx->pauseTick;
		ctx->pauseTick  = 0;
	}
	ctx->paused = false;
	Mixer_Pump();
	return 0;
}

static cc_result Audio_Poll(struct AudioContext* ctx, int* inUse) {
	cc_uint32 now;

	*inUse = 0;
	Mixer_Pump();

	if (!ctx->stream || !ctx->pendingSamples) { ctx->started = false; return 0; }

	if (!ctx->paused && ctx->started) {
		cc_uint32 consumed;
		now = SDL_GetTicks();
		if (now >= ctx->startTick) {
			consumed = (cc_uint32)((cc_uint64)(now - ctx->startTick) * ctx->sampleRate / 1000);
			if (consumed >= ctx->pendingSamples) ctx->pendingSamples = 0;
			else ctx->pendingSamples -= consumed;
			ctx->startTick = now;
		}
	}
	if (!ctx->pendingSamples) { ctx->started = false; return 0; }

	*inUse = (ctx->pendingSamples + ctx->bufSamples - 1) / ctx->bufSamples;
	if (*inUse > ctx->count) *inUse = ctx->count;
	return 0;
}


/*########################################################################################################################*
*------------------------------------------------------Stream context-----------------------------------------------------*
*#########################################################################################################################*/
cc_result StreamContext_SetFormat(struct AudioContext* ctx, int channels, int sampleRate, int playbackRate) {
	return Audio_SetFormat(ctx, channels, sampleRate, playbackRate);
}

cc_result StreamContext_Enqueue(struct AudioContext* ctx, struct AudioChunk* chunk) {
	return Audio_QueueChunk(ctx, chunk);
}

cc_result StreamContext_Play(struct AudioContext* ctx) {
	return Audio_Play(ctx);
}

cc_result StreamContext_Pause(struct AudioContext* ctx) {
	ctx->paused    = true;
	ctx->pauseTick = SDL_GetTicks();
	return 0;
}

cc_result StreamContext_Update(struct AudioContext* ctx, int* inUse) {
	return Audio_Poll(ctx, inUse);
}


/*########################################################################################################################*
*------------------------------------------------------Sound context------------------------------------------------------*
*#########################################################################################################################*/
cc_bool SoundContext_FastPlay(struct AudioContext* ctx, struct AudioData* data) {
	if (ctx->paused || ctx->pendingSamples) return false;
	return ctx->stream && ctx->channels == data->channels
		&& ctx->sampleRate == Audio_AdjustSampleRate(data->sampleRate, data->rate);
}

cc_result SoundContext_PlayData(struct AudioContext* ctx, struct AudioData* data) {
	cc_result res;

	if ((res = Audio_SetFormat(ctx, data->channels, data->sampleRate, data->rate))) return res;
	/* context is being reused for a new sound - drop any leftover data */
	Audio_ResetStream(ctx);
	if ((res = Audio_QueueChunk(ctx, &data->chunk))) return res;
	return Audio_Play(ctx);
}

cc_result SoundContext_PollBusy(struct AudioContext* ctx, cc_bool* isBusy) {
	int inUse = 1;
	cc_result res;
	if ((res = Audio_Poll(ctx, &inUse))) return res;

	*isBusy = inUse > 0;
	return 0;
}


/*########################################################################################################################*
*--------------------------------------------------------Audio misc-------------------------------------------------------*
*#########################################################################################################################*/
cc_bool Audio_DescribeError(cc_result res, cc_string* dst) {
	const char* err = SDL_GetError();
	if (err && err[0]) { String_AppendConst(dst, err); return true; }
	return false;
}
#endif
