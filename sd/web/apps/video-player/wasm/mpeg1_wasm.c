// mpeg1_wasm.c — glue fra pl_mpeg (MIT, phoboslab) e il player MPEG-1 del Video Player web.
//
// Il browser non sa riprodurre MPEG-1 (.mpg program stream / .m1v elementary stream): questo
// modulo, compilato in WebAssembly, fa demux + decodifica video (YCbCr planare) + decodifica
// audio MP2 (float) — il JS fa tutto il resto (rete, clock, WebGL, WebAudio).
//
// Modello a flusso: il JS scrive i byte man mano che arrivano (mp_input + mp_write) in un buffer
// ad anello di pl_mpeg; per il seek il JS calcola l'offset, chiama mp_reset e riprende a scrivere
// da lì. I tempi (secondi, 0 = primo fotogramma del file) vengono dal PTS del primo pacchetto
// dopo ogni reset + contatore di fotogrammi/campioni, così restano esatti anche dopo un seek.
// Dopo un reset i fotogrammi sono marcati "non validi" finché non arriva un I-frame (i P-frame
// senza riferimento sarebbero spazzatura).
//
// Nessuna dipendenza dallo stdio: niente import WASI a parte (eventualmente) quelli della libc.
// Ricompilare con build_mpeg1_wasm.sh (o .ps1) in questa cartella.

#define PLM_NO_STDIO
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

#define EXPORT(name) __attribute__((export_name(#name)))

typedef struct {
	plm_buffer_t *src;          // byte grezzi (PS o ES) scritti dal JS
	plm_demux_t  *demux;        // NULL in modalità ES
	plm_buffer_t *vbuf;
	plm_video_t  *video;
	plm_buffer_t *abuf;
	plm_audio_t  *audio;
	int es;                     // 1 = elementary stream video (.m1v)
	int vtype, atype;           // tipi di pacchetto instradati (0 = spento)

	double start_pts;           // PTS del primo fotogramma del file (fornito dal JS)
	int    v_base_set, a_base_set;
	double v_base, a_base;      // tempo del primo fotogramma/campione dopo l'ultimo reset
	long   v_count, a_count;    // fotogrammi / campioni prodotti dopo l'ultimo reset

	int ref_old_valid, ref_new_valid;  // validità dei due fotogrammi di riferimento

	plm_frame_t   *frame;       // ultimo fotogramma restituito
	int            frame_valid;
	double         frame_time;
	plm_samples_t *samples;     // ultimo blocco audio restituito
	double         samples_time;

	int    src_ended;           // il JS ha segnalato la fine del file (mp_end)
	double last_vpts;           // PTS video più alto visto (per la durata a fine flusso)
	double last_apts;
	uint8_t *in;                // area di scrittura per il JS
	size_t   in_cap;
} ctx_t;

// ---------------------------------------------------------------- instradamento pacchetti

static void route_packets(ctx_t *c, int requested) {
	plm_packet_t *p;
	while ((p = plm_demux_decode(c->demux))) {
		if (c->vtype && p->type == c->vtype) {
			if (p->pts != PLM_PACKET_INVALID_TS) {
				if (!c->v_base_set) { c->v_base = p->pts - c->start_pts; c->v_count = 0; c->v_base_set = 1; }
				if (p->pts > c->last_vpts) c->last_vpts = p->pts;
			}
			plm_buffer_write(c->vbuf, p->data, p->length);
		}
		else if (c->atype && p->type == c->atype) {
			if (p->pts != PLM_PACKET_INVALID_TS) {
				if (!c->a_base_set) { c->a_base = p->pts - c->start_pts; c->a_count = 0; c->a_base_set = 1; }
				if (p->pts > c->last_apts) c->last_apts = p->pts;
			}
			plm_buffer_write(c->abuf, p->data, p->length);
		}
		if (p->type == requested) {
			return;
		}
	}
	if (plm_demux_has_ended(c->demux)) {
		if (c->vbuf) plm_buffer_signal_end(c->vbuf);
		if (c->abuf) plm_buffer_signal_end(c->abuf);
	}
}

static void load_video(plm_buffer_t *b, void *user) {
	PLM_UNUSED(b);
	ctx_t *c = (ctx_t *)user;
	route_packets(c, c->vtype);
}

static void load_audio(plm_buffer_t *b, void *user) {
	PLM_UNUSED(b);
	ctx_t *c = (ctx_t *)user;
	route_packets(c, c->atype);
}

// Il decoder scarta i byte già letti (discard_read_bytes) e così "length" non coincide più con
// il total_size fissato da signal_end: senza questo richiamo un .m1v non arriverebbe mai alla fine.
static void load_src(plm_buffer_t *b, void *user) {
	ctx_t *c = (ctx_t *)user;
	if (c->src_ended) plm_buffer_signal_end(b);
}

// ---------------------------------------------------------------- ciclo di vita

EXPORT(mp_create)
ctx_t *mp_create(int es) {
	ctx_t *c = (ctx_t *)calloc(1, sizeof(ctx_t));
	if (!c) return NULL;
	c->es = es;
	c->last_vpts = -1;
	c->last_apts = -1;
	c->src = plm_buffer_create_with_capacity(1024 * 1024);
	plm_buffer_set_load_callback(c->src, load_src, c);
	if (es) {
		c->vbuf = c->src;
		c->video = plm_video_create_with_buffer(c->src, FALSE);
		c->v_base_set = 1;
	}
	else {
		c->demux = plm_demux_create(c->src, FALSE);
		c->vtype = PLM_DEMUX_PACKET_VIDEO_1;
		c->atype = PLM_DEMUX_PACKET_AUDIO_1;
		c->vbuf = plm_buffer_create_with_capacity(PLM_BUFFER_DEFAULT_SIZE);
		plm_buffer_set_load_callback(c->vbuf, load_video, c);
		c->video = plm_video_create_with_buffer(c->vbuf, TRUE);
		c->abuf = plm_buffer_create_with_capacity(PLM_BUFFER_DEFAULT_SIZE);
		plm_buffer_set_load_callback(c->abuf, load_audio, c);
		c->audio = plm_audio_create_with_buffer(c->abuf, TRUE);
	}
	return c;
}

EXPORT(mp_destroy)
void mp_destroy(ctx_t *c) {
	if (!c) return;
	if (c->video) plm_video_destroy(c->video);   // distrugge anche vbuf (tranne in ES)
	if (c->audio) plm_audio_destroy(c->audio);
	if (c->demux) plm_demux_destroy(c->demux);
	plm_buffer_destroy(c->src);
	free(c->in);
	free(c);
}

// Tipo del pacchetto video/audio da decodificare (0xE0 / 0xC0..0xC3; 0 = disattiva).
EXPORT(mp_set_streams)
void mp_set_streams(ctx_t *c, int vtype, int atype) {
	if (c->es) return;
	c->vtype = vtype;
	c->atype = atype;
}

// Area di memoria dove il JS copia i prossimi `len` byte; poi chiama mp_write(len).
EXPORT(mp_input)
uint8_t *mp_input(ctx_t *c, size_t len) {
	if (len > c->in_cap) {
		free(c->in);
		c->in_cap = len + 64 * 1024;
		c->in = (uint8_t *)malloc(c->in_cap);
		if (!c->in) { c->in_cap = 0; return NULL; }
	}
	return c->in;
}

EXPORT(mp_write)
size_t mp_write(ctx_t *c, size_t len) {
	if (!c->in || len > c->in_cap) return 0;
	return plm_buffer_write(c->src, c->in, len);
}

// Fine del file: non arriveranno altri byte fino al prossimo reset.
EXPORT(mp_end)
void mp_end(ctx_t *c) {
	c->src_ended = 1;
	plm_buffer_signal_end(c->src);
}

// Byte scritti ma non ancora consumati dal demuxer.
EXPORT(mp_remaining)
size_t mp_remaining(ctx_t *c) {
	return plm_buffer_get_remaining(c->src);
}

// Byte in attesa nei buffer elementari (video + audio), già estratti dal demuxer.
EXPORT(mp_queued)
size_t mp_queued(ctx_t *c) {
	if (c->es) return 0;
	return plm_buffer_get_remaining(c->vbuf) + plm_buffer_get_remaining(c->abuf);
}

// Svuota tutto per ripartire da un nuovo offset. start_pts = PTS del primo fotogramma del file.
// I tempi riprendono dal PTS del primo pacchetto che arriverà (o da mp_set_video_time in ES).
EXPORT(mp_reset)
void mp_reset(ctx_t *c, double start_pts) {
	c->start_pts = start_pts;
	c->src_ended = 0;
	if (c->es) {
		plm_video_rewind(c->video);    // riavvolge anche src (è lo stesso buffer)
		c->v_base_set = 1;
		c->v_base = 0;
	}
	else {
		plm_demux_rewind(c->demux);    // svuota src (buffer ad anello)
		plm_video_rewind(c->video);
		plm_audio_rewind(c->audio);
		c->v_base_set = 0;
		c->a_base_set = 0;
		c->v_base = 0;
		c->a_base = 0;
	}
	c->v_count = 0;
	c->a_count = 0;
	c->ref_old_valid = 0;
	c->ref_new_valid = 0;
	c->frame = NULL;
	c->samples = NULL;
}

// ES (.m1v) non ha PTS: il JS imposta il tempo stimato dopo un seek.
EXPORT(mp_set_video_time)
void mp_set_video_time(ctx_t *c, double t) {
	c->v_base = t;
	c->v_count = 0;
	c->v_base_set = 1;
}

EXPORT(mp_set_start_pts)
void mp_set_start_pts(ctx_t *c, double start_pts) {
	c->start_pts = start_pts;
}

// ---------------------------------------------------------------- informazioni

EXPORT(mp_has_video_header)
int mp_has_video_header(ctx_t *c) { return c->video && plm_video_has_header(c->video); }

EXPORT(mp_has_audio_header)
int mp_has_audio_header(ctx_t *c) { return c->audio && plm_audio_has_header(c->audio); }

EXPORT(mp_width)
int mp_width(ctx_t *c) { return c->video ? plm_video_get_width(c->video) : 0; }

EXPORT(mp_height)
int mp_height(ctx_t *c) { return c->video ? plm_video_get_height(c->video) : 0; }

EXPORT(mp_framerate)
double mp_framerate(ctx_t *c) { return c->video ? plm_video_get_framerate(c->video) : 0; }

EXPORT(mp_pixel_aspect)
double mp_pixel_aspect(ctx_t *c) { return c->video ? plm_video_get_pixel_aspect_ratio(c->video) : 0; }

EXPORT(mp_samplerate)
int mp_samplerate(ctx_t *c) { return c->audio ? plm_audio_get_samplerate(c->audio) : 0; }

// Canali del flusso MP2 (1 = mono; pl_mpeg duplica comunque su L/R).
EXPORT(mp_audio_channels)
int mp_audio_channels(ctx_t *c) {
	if (!c->audio || !plm_audio_has_header(c->audio)) return 0;
	return c->audio->mode == 3 ? 1 : 2;   // PLM_AUDIO_MODE_MONO
}

EXPORT(mp_audio_bitrate)
int mp_audio_bitrate(ctx_t *c) {
	if (!c->audio || !plm_audio_has_header(c->audio)) return 0;
	return PLM_AUDIO_BIT_RATE[c->audio->bitrate_index];
}

// Flussi dichiarati nel system header del program stream: (video << 8) | audio. -1 se non letto.
EXPORT(mp_declared_streams)
int mp_declared_streams(ctx_t *c) {
	if (c->es || !plm_demux_has_headers(c->demux)) return -1;
	return (plm_demux_get_num_video_streams(c->demux) << 8) | plm_demux_get_num_audio_streams(c->demux);
}

EXPORT(mp_last_video_pts)
double mp_last_video_pts(ctx_t *c) { return c->last_vpts; }

EXPORT(mp_last_audio_pts)
double mp_last_audio_pts(ctx_t *c) { return c->last_apts; }

// ---------------------------------------------------------------- video

static double fps_of(ctx_t *c) {
	double f = plm_video_get_framerate(c->video);
	return f > 0 ? f : 25.0;
}

// Copia di plm_video_decode() che tiene traccia della validità dei riferimenti.
static plm_frame_t *video_decode_tracked(ctx_t *c, int *valid) {
	plm_video_t *self = c->video;
	if (!plm_video_has_header(self)) {
		return NULL;
	}

	plm_frame_t *frame = NULL;
	int fvalid = 0;
	do {
		if (self->start_code != PLM_START_PICTURE) {
			self->start_code = plm_buffer_find_start_code(self->buffer, PLM_START_PICTURE);
			if (self->start_code == -1) {
				if (
					self->has_reference_frame &&
					!self->assume_no_b_frames &&
					plm_buffer_has_ended(self->buffer) && (
						self->picture_type == PLM_VIDEO_PICTURE_TYPE_INTRA ||
						self->picture_type == PLM_VIDEO_PICTURE_TYPE_PREDICTIVE
					)
				) {
					self->has_reference_frame = FALSE;
					frame = &self->frame_backward;
					fvalid = c->ref_new_valid;
					break;
				}
				return NULL;
			}
		}

		if (
			plm_buffer_has_start_code(self->buffer, PLM_START_PICTURE) == -1 &&
			!plm_buffer_has_ended(self->buffer)
		) {
			return NULL;
		}
		plm_buffer_discard_read_bytes(self->buffer);

		plm_video_decode_picture(self);
		int t = self->picture_type;

		if (t == PLM_VIDEO_PICTURE_TYPE_B && !self->assume_no_b_frames) {
			frame = &self->frame_current;
			fvalid = c->ref_old_valid && c->ref_new_valid;
		}
		else {
			int newv = (t == PLM_VIDEO_PICTURE_TYPE_INTRA) ? 1 : c->ref_new_valid;
			c->ref_old_valid = c->ref_new_valid;
			c->ref_new_valid = newv;
			if (self->assume_no_b_frames) {
				frame = &self->frame_backward;
				fvalid = newv;
			}
			else if (self->has_reference_frame) {
				frame = &self->frame_forward;
				fvalid = c->ref_old_valid;
			}
			else {
				self->has_reference_frame = TRUE;
			}
		}
	} while (!frame);

	*valid = fvalid;
	return frame;
}

// Decodifica il prossimo fotogramma. 1 = pronto (vedi mp_frame_*), 0 = servono altri byte,
// -1 = fine del flusso.
EXPORT(mp_decode_video)
int mp_decode_video(ctx_t *c) {
	if (!c->video || (!c->es && !c->vtype)) return -1;
	int valid = 0;
	plm_frame_t *f = video_decode_tracked(c, &valid);
	if (!f) {
		if (plm_buffer_has_ended(c->vbuf) && (c->es || plm_demux_has_ended(c->demux))) return -1;
		return 0;
	}
	c->frame = f;
	c->frame_valid = valid && c->v_base_set;
	c->frame_time = c->v_base + (double)c->v_count / fps_of(c);
	f->time = c->frame_time;
	c->v_count++;
	return 1;
}

// Tempo del prossimo fotogramma che verrà restituito (-1 se ancora sconosciuto).
EXPORT(mp_video_next_time)
double mp_video_next_time(ctx_t *c) {
	if (!c->v_base_set || !plm_video_has_header(c->video)) return -1;
	return c->v_base + (double)c->v_count / fps_of(c);
}

EXPORT(mp_frame_valid)
int mp_frame_valid(ctx_t *c) { return c->frame ? c->frame_valid : 0; }

EXPORT(mp_frame_time)
double mp_frame_time(ctx_t *c) { return c->frame ? c->frame_time : -1; }

EXPORT(mp_frame_y)
uint8_t *mp_frame_y(ctx_t *c) { return c->frame ? c->frame->y.data : NULL; }

EXPORT(mp_frame_cb)
uint8_t *mp_frame_cb(ctx_t *c) { return c->frame ? c->frame->cb.data : NULL; }

EXPORT(mp_frame_cr)
uint8_t *mp_frame_cr(ctx_t *c) { return c->frame ? c->frame->cr.data : NULL; }

// Larghezza/altezza dei piani (multipli di 16/8: vanno ritagliati a mp_width/mp_height).
EXPORT(mp_luma_width)
int mp_luma_width(ctx_t *c) { return c->frame ? (int)c->frame->y.width : 0; }

EXPORT(mp_luma_height)
int mp_luma_height(ctx_t *c) { return c->frame ? (int)c->frame->y.height : 0; }

EXPORT(mp_chroma_width)
int mp_chroma_width(ctx_t *c) { return c->frame ? (int)c->frame->cb.width : 0; }

EXPORT(mp_chroma_height)
int mp_chroma_height(ctx_t *c) { return c->frame ? (int)c->frame->cb.height : 0; }

// Fallback senza WebGL: converte l'ultimo fotogramma in RGBA (stride in byte).
EXPORT(mp_frame_to_rgba)
void mp_frame_to_rgba(ctx_t *c, uint8_t *dst, int stride) {
	if (c->frame) plm_frame_to_rgba(c->frame, dst, stride);
}

// ---------------------------------------------------------------- audio

// Decodifica un blocco audio (1152 campioni stereo interleaved float). 1/0/-1 come il video.
EXPORT(mp_decode_audio)
int mp_decode_audio(ctx_t *c) {
	if (!c->audio || !c->atype) return -1;
	plm_samples_t *s = plm_audio_decode(c->audio);
	if (!s) {
		if (plm_buffer_has_ended(c->abuf) && plm_demux_has_ended(c->demux)) return -1;
		return 0;
	}
	int sr = plm_audio_get_samplerate(c->audio);
	c->samples = s;
	c->samples_time = c->a_base + (sr > 0 ? (double)c->a_count / sr : 0);
	c->a_count += PLM_AUDIO_SAMPLES_PER_FRAME;
	return 1;
}

EXPORT(mp_audio_valid)
int mp_audio_valid(ctx_t *c) { return c->a_base_set; }

EXPORT(mp_samples_ptr)
float *mp_samples_ptr(ctx_t *c) { return c->samples ? c->samples->interleaved : NULL; }

EXPORT(mp_samples_time)
double mp_samples_time(ctx_t *c) { return c->samples ? c->samples_time : -1; }

EXPORT(mp_samples_count)
int mp_samples_count(void) { return PLM_AUDIO_SAMPLES_PER_FRAME; }

EXPORT(mp_audio_next_time)
double mp_audio_next_time(ctx_t *c) {
	if (!c->audio || !c->a_base_set) return -1;
	int sr = plm_audio_get_samplerate(c->audio);
	return c->a_base + (sr > 0 ? (double)c->a_count / sr : 0);
}

// ---------------------------------------------------------------- sonda su un blocco

// Analizza un blocco di byte qualunque (testa, coda, o un punto di seek) senza toccare il
// contesto di riproduzione. what: 0 = maschera dei flussi visti (bit0 video E0, bit1..4 audio
// C0..C3), 1 = primo PTS del tipo `type`, 2 = ultimo PTS del tipo `type`, 3 = offset del primo
// pacchetto `type` con PTS (per agganciarsi a un punto di seek), 4 = offset del primo pacchetto
// video che contiene un I-frame. Ritorna -1 se non trovato.
EXPORT(mp_scan)
double mp_scan(uint8_t *bytes, size_t len, int what, int type) {
	plm_buffer_t *b = plm_buffer_create_with_memory(bytes, len, FALSE);
	plm_demux_t *d = plm_demux_create(b, TRUE);
	// Il blocco può non contenere pack/system header (coda o metà file): li diamo per letti.
	d->has_pack_header = TRUE;
	d->has_system_header = TRUE;
	d->has_headers = TRUE;
	plm_demux_buffer_seek(d, 0);

	double result = -1;
	int mask = 0;
	plm_packet_t *p;
	for (;;) {
		size_t before = plm_buffer_tell(b);
		p = plm_demux_decode(d);
		if (!p) break;
		if (p->type == PLM_DEMUX_PACKET_VIDEO_1) mask |= 1;
		else if (p->type >= PLM_DEMUX_PACKET_AUDIO_1 && p->type <= PLM_DEMUX_PACKET_AUDIO_4) mask |= 2 << (p->type - PLM_DEMUX_PACKET_AUDIO_1);
		if (p->type != type) continue;
		if (what == 1 && p->pts != PLM_PACKET_INVALID_TS) { result = p->pts; break; }
		if (what == 2 && p->pts != PLM_PACKET_INVALID_TS) { result = p->pts; }
		if (what == 3 && p->pts != PLM_PACKET_INVALID_TS) {
			// L'inizio del pacchetto è prima dell'intestazione PES: cerca indietro lo start code.
			size_t pos = (size_t)(p->data - bytes);
			size_t lim = pos > 64 ? pos - 64 : 0;
			result = (double)before;
			for (size_t i = pos; i-- > lim; ) {
				if (i + 3 < len && bytes[i] == 0 && bytes[i+1] == 0 && bytes[i+2] == 1 && bytes[i+3] == (uint8_t)type) { result = (double)i; break; }
			}
			break;
		}
		if (what == 4) {
			int found = 0;
			for (size_t i = 0; p->length >= 6 && i < p->length - 6; i++) {
				if (p->data[i] == 0 && p->data[i+1] == 0 && p->data[i+2] == 1 && p->data[i+3] == 0) {
					if ((p->data[i+5] & 0x38) == 8) found = 1;
					break;
				}
			}
			if (found && p->pts != PLM_PACKET_INVALID_TS) {
				size_t pos = (size_t)(p->data - bytes);
				size_t lim = pos > 64 ? pos - 64 : 0;
				result = (double)before;
				for (size_t i = pos; i-- > lim; ) {
					if (i + 3 < len && bytes[i] == 0 && bytes[i+1] == 0 && bytes[i+2] == 1 && bytes[i+3] == (uint8_t)type) { result = (double)i; break; }
				}
				break;
			}
		}
	}
	plm_demux_destroy(d);
	return what == 0 ? (double)mask : result;
}

// PTS del pacchetto `type` il cui inizio è all'offset restituito da mp_scan(what=3/4).
EXPORT(mp_scan_pts_at)
double mp_scan_pts_at(uint8_t *bytes, size_t len, size_t offset, int type) {
	if (offset >= len) return -1;
	return mp_scan(bytes + offset, len - offset, 1, type);
}

// ---------------------------------------------------------------- memoria per il JS

EXPORT(mp_malloc)
void *mp_malloc(size_t n) { return malloc(n); }

EXPORT(mp_free)
void mp_free(void *p) { free(p); }
