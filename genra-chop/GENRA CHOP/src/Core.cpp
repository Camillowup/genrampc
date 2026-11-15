// SPDX-License-Identifier: GPL-3.0-only
// GENRA CHOP: the generator and the live rendering (see Core.hpp).
#include "Core.hpp"
#include <algorithm>
#include <cstdio>

namespace hh {

const char* const QUALITY_NAMES[NUM_QUALITIES] = {"", "m", "7", "maj7", "m7", "m9", "dim", "sus4"};
const int QUALITY_TONES[NUM_QUALITIES][5] = {
	{0, 4, 7, -1, -1}, {0, 3, 7, -1, -1}, {0, 4, 7, 10, -1}, {0, 4, 7, 11, -1},
	{0, 3, 7, 10, -1}, {0, 3, 7, 10, 14}, {0, 3, 6, -1, -1}, {0, 5, 7, -1, -1},
};
const char* const NOTE_NAMES[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
const char* const BASS_MODES[4] = {"FOLLOW", "ROOT+5", "WALK", "808"};
const char* const MEL_MODES[4] = {"STAB", "LEAD", "ARP", "RIFF"};
const char* const PAD_MODES[3] = {"SUSTAIN", "STAB", "SWELL"};

static const int SCALES[NUM_SCALES][7] = {
	{0, 2, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 9, 10}, {0, 2, 4, 5, 7, 9, 11}, {0, 1, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 8, 11},
};

static int pcOf(int n) { return ((n % 12) + 12) % 12; }

void chordName(const Chord& c, char* buf, int len) {
	if (!c.valid())
		snprintf(buf, len, "-");
	else
		snprintf(buf, len, "%s%s", NOTE_NAMES[pcOf(c.root)], QUALITY_NAMES[c.quality]);
}

void keyName(const Key& k, char* buf, int len) { snprintf(buf, len, "%s%s", NOTE_NAMES[pcOf(k.root)], k.minor ? "m" : ""); }

int scaleStep(int scale, int d) {
	int oct = d >= 0 ? d / 7 : -((-d + 6) / 7);
	int i = d - oct * 7;
	return SCALES[scale][i] + 12 * oct;
}

void Pattern::clear() { *this = Pattern(); }

// ---------------------------------------------------------------- harmony helpers
/** The scale a style plays in a key (a major key with a minor style plays major, and the other way round). */
static int scaleFor(const Style& st, const Key& k) {
	bool styleMinor = st.scale != SC_MAJOR;
	if (k.minor == styleMinor)
		return st.scale;
	return k.minor ? SC_MINOR : SC_MAJOR;
}

/** The chord on scale degree d (1-based) in colour 0/1/2. */
static Chord degreeChord(int scale, int keyRoot, int d, int color) {
	int i = d - 1;
	int r = scaleStep(scale, i);
	int third = scaleStep(scale, i + 2) - r;
	int fifth = scaleStep(scale, i + 4) - r;
	int sev = scaleStep(scale, i + 6) - r;
	Chord c;
	c.root = pcOf(keyRoot + r);
	if (fifth == 6)
		c.quality = Q_DIM;
	else if (third == 4)
		c.quality = color == 0 ? Q_MAJ : (sev == 10 ? Q_DOM7 : Q_MAJ7);
	else
		c.quality = color == 0 ? Q_MIN : (color == 2 && sev == 10 ? Q_MIN9 : Q_MIN7);
	return c;
}

/** Chord tones of a chord as pitch classes (up to 5); returns the count. */
static int chordTones(const Chord& c, int* out) {
	int n = 0;
	if (!c.valid())
		return 0;
	for (int i = 0; i < 5; i++)
		if (QUALITY_TONES[c.quality][i] >= 0)
			out[n++] = c.root + QUALITY_TONES[c.quality][i];
	return n;
}

/** The MIDI note of pitch class pc nearest to `near`. */
static int nearest(int pc, int near) {
	int n = near - pcOf(near) + pcOf(pc);
	if (n - near > 6)
		n -= 12;
	if (near - n > 6)
		n += 12;
	return n;
}

/** The note of the key's scale nearest to n. */
static int snapScale(int n, int scale, int keyRoot) {
	for (int d = 0; d < 7; d++) {
		for (int s = -1; s <= 1; s += 2) {
			int m = n + d * s;
			int rel = pcOf(m - keyRoot);
			for (int i = 0; i < 7; i++)
				if (SCALES[scale][i] == rel)
					return m;
		}
	}
	return n;
}

int scaleOf(const Settings& set) { return scaleFor(STYLES[std::max(0, std::min(NUM_STYLES - 1, set.style))], set.key); }
bool inScale(int note, int scale, int keyRoot) {
	int rel = pcOf(note - keyRoot);
	for (int i = 0; i < 7; i++)
		if (SCALES[scale][i] == rel)
			return true;
	return false;
}
int snapToScale(int note, int scale, int keyRoot) { return snapScale(note, scale, keyRoot); }
int tonesOf(const Chord& c, int* out) { return chordTones(c, out); }

Chord padChord(const SampleInfo& si, const PadSet* ps, int pad) {
	Chord c;
	c.quality = Q_NONE;
	if (pad < 0 || pad >= PADS)
		return c;
	if (ps && ps[pad].chordQ >= NUM_QUALITIES)
		return c; // set by hand: no chord
	if (ps && ps[pad].chordQ >= 0) {
		c.root = ps[pad].chordRoot;
		c.quality = ps[pad].chordQ;
		return c;
	}
	if (pad < si.count && si.chop[pad].conf > 0.15f)
		return si.chop[pad].chord;
	return c;
}

Chord chordAt(const Pattern& p, const SampleInfo& si, const PadSet* pads, int s) {
	s = ((s % LEN) + LEN) % LEN;
	if (si.count > 0) {
		for (int back = 0; back < STEPS; back++) {
			int i = ((s - back) % LEN + LEN) % LEN;
			const ChopEv& e = p.chop[i];
			if (e.pad < 0)
				continue;
			if (back < e.len + 0.5f) {
				Chord c = padChord(si, pads, e.pad);
				if (c.valid()) {
					c.root = pcOf(c.root + e.pitch + (pads ? pads[e.pad].pitch : 0));
					return c;
				}
			}
			break;
		}
	}
	return p.bar[s / STEPS];
}

// ---------------------------------------------------------------- drums
static void genDrums(Pattern& p, const Style& st, Rng& rng) {
	Hit A[DRUMS][STEPS];
	for (int v = 0; v < DRUMS; v++) {
		const char* s = st.drum[v];
		int n = (int) strlen(s);
		for (int i = 0; i < STEPS; i++) {
			char c = i < n ? s[i] : '.';
			Hit h;
			if (c == 'X')
				h.vel = 1.f, h.prio = 0.f;
			else if (c == 'x')
				h.vel = 0.8f, h.prio = 0.1f;
			else if (c == 'o')
				h.vel = 0.45f, h.prio = 0.55f;
			A[v][i] = h;
		}
	}
	// what DENS adds on top: kick pushes, hat ghosts, an open hat, rim ghosts
	static const int KICK_EXTRA[] = {3, 7, 9, 11, 14, 15};
	for (int k : KICK_EXTRA)
		if (A[KICK][k].vel == 0.f && rng.chance(0.4f))
			A[KICK][k].vel = rng.range(0.5f, 0.75f), A[KICK][k].prio = rng.range(0.6f, 0.9f);
	// hats come early with DENS (from about 30-40 %): boom bap gets quiet ghost hats between the eighths, trap
	// gets its 16ths sooner; a style with hats only on the quarters gets the missing eighths first
	{
		bool trapH = st.hatRoll > 0.f || st.bass == BM_808;
		bool anyCH = false;
		for (int i = 0; i < STEPS; i++)
			anyCH = anyCH || A[CH][i].vel > 0.f;
		if (anyCH)
			for (int i = 0; i < STEPS; i += 2)
				if (A[CH][i].vel == 0.f && rng.chance(0.7f))
					A[CH][i].vel = rng.range(0.4f, 0.55f), A[CH][i].prio = rng.range(0.28f, 0.38f);
		for (int i = 1; i < STEPS; i += 2)
			if (A[CH][i].vel == 0.f && A[CH][i - 1].vel > 0.f && rng.chance(0.8f)) {
				if (trapH)
					A[CH][i].vel = rng.range(0.32f, 0.48f), A[CH][i].prio = rng.range(0.3f, 0.45f);
				else
					A[CH][i].vel = rng.range(0.18f, 0.3f), A[CH][i].prio = rng.range(0.32f, 0.48f);
			}
	}
	if (A[OH][14].vel == 0.f && rng.chance(0.5f))
		A[OH][14].vel = 0.6f, A[OH][14].prio = 0.75f;
	if (A[SNARE][4].vel > 0.f || A[SNARE][8].vel > 0.f) {
		int g = rng.chance(0.5f) ? 7 : 15;
		if (A[SNARE][g].vel == 0.f)
			A[SNARE][g].vel = 0.35f, A[SNARE][g].prio = 0.65f;
	}
	// more with DENS, by the kind of style (only on voices the style leaves empty, all low priority: DENS to the
	// right brings them): rim ghosts, a shaker / tambourine on PERC, open hats, a clap on the snare
	{
		bool trap = st.hatRoll > 0.f || st.bass == BM_808, funk = st.bass == BM_FUNK;
		auto empty = [&](int v) {
			for (int i = 0; i < STEPS; i++)
				if (A[v][i].vel > 0.f)
					return false;
			return true;
		};
		auto add = [&](int v, int i, float vel, float prio) {
			if (A[v][i].vel == 0.f)
				A[v][i].vel = vel, A[v][i].prio = prio;
		};
		if (empty(RIM)) {
			static const int BB[] = {3, 7, 11, 14}, TR[] = {6, 10, 14};
			const int* at = trap ? TR : BB;
			int n = trap ? 3 : 4;
			for (int k = 0; k < n; k++)
				if (rng.chance(0.5f))
					add(RIM, at[k], rng.range(0.25f, 0.4f), rng.range(0.6f, 0.9f));
		}
		if (empty(PERC)) {
			if (trap) {
				static const int TP[] = {3, 7, 10, 13};
				for (int i : TP)
					if (rng.chance(0.4f))
						add(PERC, i, rng.range(0.3f, 0.45f), rng.range(0.76f, 0.9f));
			}
			else {
				// tambourine: rarer, quieter, only from about 75 % DENS
				static const int TB[] = {6, 14};
				for (int i : TB)
					if (rng.chance(0.6f))
						add(PERC, i, rng.range(0.15f, 0.25f), rng.range(0.76f, 0.9f));
				if (rng.chance(0.35f))
					for (int i = 0; i < STEPS; i += 2)
						add(PERC, i, 0.12f, 0.94f); // a shaker on the eighths, last of all
			}
		}
		if (funk || trap) {
			static const int OHS[] = {6, 14};
			for (int i : OHS)
				if (rng.chance(0.5f))
					add(OH, i, rng.range(0.45f, 0.65f), rng.range(0.6f, 0.85f));
		}
	}
	// the clap layer is no longer in the bar template: it comes only now and then (bars 2 and 4, before the fill)
	// and only with high DENS (from about 80 %); styles with their own clap keep it
	bool clapLayer = true;
	for (int i = 0; i < STEPS; i++)
		clapLayer = clapLayer && A[CLAP][i].vel == 0.f;
	{
		bool trapC = st.hatRoll > 0.f || st.bass == BM_808, funkC = st.bass == BM_FUNK;
		clapLayer = clapLayer && rng.chance(trapC || funkC ? 0.7f : 0.6f);
	}
	// the four bars: A A A A+fill (bar 2 a little different)
	for (int b = 0; b < BARS; b++)
		for (int v = 0; v < DRUMS; v++)
			for (int i = 0; i < STEPS; i++) {
				Hit h = A[v][i];
				if (h.vel > 0.f) {
					h.vel = std::min(1.f, std::max(0.1f, h.vel + rng.range(-0.07f, 0.07f)));
					h.off = rng.range(-st.humanize, st.humanize);
					if (st.dilla > 0.f) {
						if (v == SNARE || v == CLAP)
							h.off += st.dilla * rng.range(0.15f, 0.4f);
						else if (v == KICK)
							h.off += st.dilla * rng.range(-0.3f, 0.2f);
						else
							h.off += st.dilla * rng.range(-0.25f, 0.25f);
					}
					h.off = std::max(-0.45f, std::min(0.45f, h.off));
					if (v == CH && st.hatRoll > 0.f && rng.chance(st.hatRoll * (b == 3 ? 0.7f : 0.38f)))
						h.ratchet = (uint8_t) (2 + rng.below(3)), h.prio = std::min(h.prio, 0.3f);
				}
				p.drum[v][b * STEPS + i] = h;
			}
	// bar 2: one kick moves
	if (rng.chance(0.6f)) {
		int from = -1;
		for (int i = 9; i < STEPS; i++)
			if (p.drum[KICK][STEPS + i].vel > 0.f && p.drum[KICK][STEPS + i].prio > 0.f)
				from = i;
		if (from > 0 && from + 1 < STEPS) {
			std::swap(p.drum[KICK][STEPS + from], p.drum[KICK][STEPS + from + 1]);
		}
	}
	// bar 4: a fill
	int b4 = 3 * STEPS;
	if (rng.chance(0.75f)) {
		int kind = rng.below(3);
		for (int i = 12; i < STEPS; i++) {
			Hit& s = p.drum[SNARE][b4 + i];
			if (kind == 0 && s.vel == 0.f && rng.chance(0.6f))
				s.vel = rng.range(0.4f, 0.8f), s.prio = 0.3f;
			if (kind == 1) {
				Hit& k = p.drum[KICK][b4 + i];
				if (k.vel == 0.f && rng.chance(0.5f))
					k.vel = 0.7f, k.prio = 0.3f;
			}
			if (kind == 2) {
				Hit& t = p.drum[TOM][b4 + i];
				t.vel = rng.range(0.6f, 0.9f), t.prio = 0.35f;
			}
		}
	}
	// with DENS far right: a tom fill into the next round (if bar 4 has none yet)
	{
		bool toms = false;
		for (int i = 12; i < STEPS; i++)
			toms = toms || p.drum[TOM][b4 + i].vel > 0.f;
		if (!toms)
			for (int i = 13; i < STEPS; i++)
				if (rng.chance(0.7f)) {
					Hit& t = p.drum[TOM][b4 + i];
					t.vel = rng.range(0.5f, 0.8f), t.prio = 0.85f, t.off = 0.f;
				}
	}
	// the clap layer: on the last main snare of bar 2 and / or bar 4
	if (clapLayer) {
		for (int b = 1; b < BARS; b += 2) {
			if (!rng.chance(b == 3 ? 0.8f : 0.6f))
				continue;
			int last = -1;
			for (int i = 0; i < STEPS; i++) {
				const Hit& sn = p.drum[SNARE][b * STEPS + i];
				if (sn.vel > 0.f && sn.prio <= 0.1f)
					last = i;
			}
			if (last >= 0) {
				Hit& c = p.drum[CLAP][b * STEPS + last];
				if (c.vel == 0.f) {
					c = p.drum[SNARE][b * STEPS + last];
					c.vel = rng.range(0.5f, 0.7f);
					c.prio = rng.range(0.8f, 0.95f);
					c.ratchet = 1;
				}
			}
		}
	}
	// an open hat into bar 1 (end of bar 4), more often and earlier with DENS
	if (rng.chance(0.75f)) {
		Hit& o = p.drum[OH][b4 + 14];
		if (o.vel == 0.f)
			o.vel = rng.range(0.5f, 0.65f), o.prio = rng.range(0.38f, 0.5f), o.off = 0.f;
	}
	// a crash-ish open hat at the top of bar 1 (in the next cycle) now and then
	if (rng.chance(0.3f))
		p.drum[OH][0].vel = 0.7f, p.drum[OH][0].prio = 0.5f;
}

// ---------------------------------------------------------------- chords per bar
static void genChords(Pattern& p, const Style& st, const Settings& set, const SampleInfo& si, const PadSet* pads,
                      Rng& rng) {
	int scale = scaleFor(st, set.key);
	int deg[BARS] = {1, 1, 1, 1};
	sscanf(st.prog, "%d %d %d %d", &deg[0], &deg[1], &deg[2], &deg[3]);
	// a little variety: bar 4 sometimes turns elsewhere, bar 2 sometimes swaps with bar 3
	if (rng.chance(0.3f)) {
		static const int TURN[] = {5, 7, 4, 2};
		deg[3] = TURN[rng.below(4)];
	}
	if (rng.chance(0.2f))
		std::swap(deg[1], deg[2]);
	for (int b = 0; b < BARS; b++)
		p.bar[b] = degreeChord(scale, set.key.root, deg[b], st.color);
	// with a sample: the chop that sounds longest in a bar gives its chord
	if (si.count > 0) {
		for (int b = 0; b < BARS; b++) {
			float best = 0.f;
			Chord bc;
			bc.quality = Q_NONE;
			for (int i = 0; i < STEPS; i++) {
				const ChopEv& e = p.chop[b * STEPS + i];
				if (e.pad < 0)
					continue;
				Chord c = padChord(si, pads, e.pad);
				if (c.valid() && e.len > best) {
					best = e.len;
					bc = c;
					bc.root = pcOf(c.root + e.pitch + (pads ? pads[e.pad].pitch : 0));
				}
			}
			if (bc.valid())
				p.bar[b] = bc;
		}
	}
}

// ---------------------------------------------------------------- bass
static void addNote(Note* line, int s, int pitch, float len, float vel, float prio) {
	if (s < 0 || s >= LEN)
		return;
	Note& n = line[s];
	n.pitch = (int8_t) std::max(0, std::min(127, pitch));
	n.len = len;
	n.vel = vel;
	n.prio = prio;
	n.off = 0.f;
	n.acc = false;
	n.slide = false;
}

static int bassNote(int pc) {
	int n = 36 + pcOf(pc);
	if (n > 42)
		n -= 12;
	return n;
}

/** What DENS brings to the bass, and how long its notes ring.
 *  - at least two notes per bar (none of the bars stays almost empty)
 *  - extras with graded priority: a push before the snare (0.3-0.45), octave jumps (0.5-0.65),
 *    walk-ups into the next chord (0.6-0.75), 16th ghosts (0.8-0.95)
 *  - the core notes ring up to the next core note (Premier-style East Coast stays short) */
static void bassLayer(Pattern& p, const Style& st, const Settings& set, const SampleInfo& si, const PadSet* pads,
                      Rng& rng, int mode, int scale) {
	auto empty = [&](int s) { return s >= 0 && s < LEN && p.bass[s].pitch < 0; };
	auto rootAt = [&](int s) {
		Chord c = chordAt(p, si, pads, s);
		if (!c.valid())
			c = p.bar[s / STEPS];
		return bassNote(c.root);
	};
	// at least two core notes per bar
	for (int b = 0; b < BARS; b++) {
		int core = 0;
		for (int i = 0; i < STEPS; i++)
			core += p.bass[b * STEPS + i].pitch >= 0 && p.bass[b * STEPS + i].prio == 0.f;
		static const int FILL[] = {8, 10, 6, 12};
		for (int k = 0; k < 4 && core < 2; k++) {
			int s = b * STEPS + FILL[k];
			if (empty(s)) {
				addNote(p.bass, s, rootAt(s) + (rng.chance(0.3f) ? 7 : 0), 1.f, 0.8f, 0.f);
				core++;
			}
		}
	}
	// the extras, the same in every bar (a groove), bar 4 gets its own walk-up
	int push = rng.chance(0.5f) ? 7 : 11; // the 16th before a snare / the "a" of 3
	float pushPrio = rng.range(0.3f, 0.45f);
	int jump = 1 + rng.below(3);
	float jumpPrio = rng.range(0.5f, 0.65f);
	for (int b = 0; b < BARS; b++) {
		int s0 = b * STEPS;
		if (empty(s0 + push))
			addNote(p.bass, s0 + push, rootAt(s0 + push) + (rng.chance(0.4f) ? 7 : 0), 0.9f, 0.7f, pushPrio);
		if (p.bass[s0].pitch >= 0 && empty(s0 + jump * 2)) {
			addNote(p.bass, s0 + jump * 2, p.bass[s0].pitch + 12, 0.8f, 0.65f, jumpPrio);
			if (mode == BM_808)
				p.bass[s0 + jump * 2].slide = rng.chance(0.5f);
		}
		// walk-up into the next bar's chord (bars 2 and 4, with DENS)
		if (b % 2 == 1) {
			int target = rootAt((s0 + STEPS) % LEN);
			for (int k = 0; k < 3; k++) {
				int s = s0 + 13 + k;
				if (!empty(s))
					continue;
				int n = snapScale(target - (3 - k) * 2, scale, set.key.root);
				addNote(p.bass, s, n, 0.8f, 0.6f, 0.6f + 0.05f * k);
			}
		}
		// 16th ghosts on the empty 8ths
		for (int i = 2; i < STEPS; i += 2) {
			int s = s0 + i;
			if (empty(s) && empty(s - 1) && rng.chance(0.5f)) {
				int prev = s - 1;
				while (prev > s0 && p.bass[prev].pitch < 0)
					prev--;
				int n = p.bass[prev].pitch >= 0 ? p.bass[prev].pitch : rootAt(s);
				addNote(p.bass, s, n + (rng.chance(0.3f) ? 12 : 0), 0.6f, 0.5f, rng.range(0.8f, 0.95f));
			}
		}
	}
	// how long the core notes ring: up to the next core note
	bool shortStyle = st.bass == BM_ROOTS && st.chopLoop < 0.4f; // East Coast: short and punchy
	for (int s = 0; s < LEN; s++) {
		Note& n = p.bass[s];
		if (n.pitch < 0 || n.prio > 0.f)
			continue;
		int nx = s + 1;
		while (nx < s + STEPS && (p.bass[nx % LEN].pitch < 0 || p.bass[nx % LEN].prio > 0.f))
			nx++;
		float gap = (float) (nx - s);
		if (mode == BM_808)
			n.len = std::max(n.len, std::min(gap, 8.f) - 0.2f);
		else if (shortStyle)
			n.len = std::min(gap, 2.f) - 0.15f;
		else
			n.len = std::min(gap, 6.f) - 0.25f;
	}
}

static void genBass(Pattern& p, const Style& st, const Settings& set, const SampleInfo& si, const PadSet* pads,
                    Rng& rng) {
	for (int s = 0; s < LEN; s++)
		p.bass[s] = Note();
	int mode = st.bass;
	bool fifths = false;
	switch (set.bassMode) {
		case 1: mode = BM_ROOTS, fifths = true; break;
		case 2: mode = BM_WALK; break;
		case 3: mode = BM_808; break;
		default: break;
	}
	int scale = scaleFor(st, set.key);
	if (mode == BM_ROOTS || mode == BM_808) {
		// on the kicks (the strong ones; the pushed kicks as DENS extras)
		int last = -100;
		int lastPitch = -1;
		bool alt = false;
		for (int s = 0; s < LEN; s++) {
			const Hit& k = p.drum[KICK][s];
			bool strong = k.vel > 0.f && k.prio <= 0.15f;
			bool weak = k.vel > 0.f && !strong;
			if (!strong && !weak && !(s % STEPS == 0 && mode == BM_808))
				continue;
			int nxt = s + 1;
			while (nxt < LEN && p.drum[KICK][nxt].vel == 0.f)
				nxt++;
			float len = (float) std::min(nxt - s, mode == BM_808 ? 8 : 4);
			Chord c = chordAt(p, si, pads, s);
			if (!c.valid())
				c = p.bar[s / STEPS];
			int root = bassNote(c.root);
			int pitch = root;
			if (fifths && alt)
				pitch = root + 7;
			alt = !alt;
			if (mode == BM_808 && rng.chance(0.15f))
				pitch += 12;
			float prio = strong ? 0.f : std::max(0.3f, k.prio);
			addNote(p.bass, s, pitch, mode == BM_808 ? len - 0.2f : std::min(len, 2.f) - 0.1f, strong ? 0.9f : 0.7f, prio);
			if (mode == BM_808 && lastPitch >= 0 && pitch != lastPitch && s - last <= 8 && rng.chance(0.4f))
				p.bass[s].slide = true;
			if (s % STEPS == 0)
				p.bass[s].acc = true;
			last = s;
			lastPitch = pitch;
		}
		// no strong kick on a bar's one (Drumless): the root there anyway
		for (int b = 0; b < BARS; b++)
			if (p.bass[b * STEPS].pitch < 0 || p.bass[b * STEPS].prio > 0.f) {
				Chord c = chordAt(p, si, pads, b * STEPS);
				if (!c.valid())
					c = p.bar[b];
				addNote(p.bass, b * STEPS, bassNote(c.root), mode == BM_808 ? 7.8f : 3.8f, 0.85f, 0.f);
				p.bass[b * STEPS].acc = true;
			}
		// passing notes before a strong kick (with DENS)
		if (mode == BM_ROOTS)
			for (int s = 2; s < LEN; s++)
				if (p.bass[s].pitch >= 0 && p.bass[s].prio == 0.f && p.bass[s - 1].pitch < 0 && p.bass[s - 2].pitch < 0 &&
				    rng.chance(0.35f)) {
					int pp = p.bass[s].pitch + (rng.chance(0.5f) ? 7 : -5);
					addNote(p.bass, s - 1, snapScale(pp, scale, set.key.root), 0.8f, 0.6f, 0.55f);
				}
	}
	else if (mode == BM_WALK) {
		for (int b = 0; b < BARS; b++) {
			Chord c = p.bar[b];
			Chord nx = p.bar[(b + 1) % BARS];
			int tones[5];
			int nt = chordTones(c, tones);
			int root = bassNote(c.root);
			for (int q = 0; q < 4; q++) {
				int s = b * STEPS + q * 4;
				Chord cs = chordAt(p, si, pads, s);
				if (cs.valid() && cs.root != c.root)
					root = bassNote(cs.root);
				int pitch;
				if (q == 0)
					pitch = root;
				else if (q == 3)
					pitch = bassNote(nx.root) + (rng.chance(0.5f) ? 1 : -1); // approach
				else
					pitch = nearest(nt > 0 ? tones[1 + rng.below(std::max(1, nt - 1))] : c.root, root + 5);
				addNote(p.bass, s, pitch, 3.2f, q == 0 ? 0.85f : 0.7f, 0.f);
				if (rng.chance(0.3f))
					addNote(p.bass, s + 3, snapScale(pitch + (rng.chance(0.5f) ? 2 : -2), scale, set.key.root), 0.8f, 0.5f, 0.6f);
			}
		}
	}
	else { // BM_FUNK
		static const int POS[] = {0, 3, 6, 8, 10, 11, 14};
		bool on[7];
		for (int i = 0; i < 7; i++)
			on[i] = i == 0 || rng.chance(0.55f);
		int intv[7];
		static const int CH[] = {0, 12, 7, 10, 0, 5, 12};
		for (int i = 0; i < 7; i++)
			intv[i] = i == 0 ? 0 : CH[rng.below(7)];
		for (int b = 0; b < BARS; b++)
			for (int i = 0; i < 7; i++) {
				if (!on[i] && !(b == 3 && rng.chance(0.3f)))
					continue;
				int s = b * STEPS + POS[i];
				Chord c = chordAt(p, si, pads, s);
				int pitch = bassNote(c.root) + intv[i];
				addNote(p.bass, s, pitch, i == 0 ? 2.f : 1.f - 0.15f, i == 0 ? 0.9f : 0.75f, on[i] ? 0.f : 0.5f);
				if (i == 0 || POS[i] == 10)
					p.bass[s].acc = true;
				if (intv[i] == 12 && rng.chance(0.4f))
					p.bass[s].slide = true;
			}
	}
	if (mode != BM_WALK)
		bassLayer(p, st, set, si, pads, rng, mode, scale);
}

// ---------------------------------------------------------------- melody
static void genMel(Pattern& p, const Style& st, const Settings& set, const SampleInfo& si, const PadSet* pads, Rng& rng) {
	for (int s = 0; s < LEN; s++)
		p.mel[s] = Note();
	int scale = scaleFor(st, set.key);
	int center = st.melOct;
	bool jazz = st.mel == MM_JAZZ;
	int mode = set.melMode;
	if (mode == 0) { // STAB: short chord-tone hits on a syncopated motif
		static const char* RH[] = {"x..x..x...x.x...", "x...x..x..x.x...", "..x..x...x..x.x.", "x.x...x..x...x.."};
		const char* rh = RH[rng.below(4)];
		int pick = rng.below(3);
		for (int s = 0; s < LEN; s++) {
			if (rh[s % STEPS] != 'x')
				continue;
			Chord c = chordAt(p, si, pads, s);
			int tones[5];
			int nt = chordTones(c, tones);
			if (!nt)
				continue;
			int pc = tones[(pick + (s % STEPS) / 3) % nt];
			addNote(p.mel, s, nearest(pc, center), rng.range(0.5f, 1.2f), 0.8f, (s % STEPS) < 8 ? 0.f : 0.3f);
		}
	}
	else if (mode == 1) { // LEAD: long legato notes, a two-bar line A B A B'
		int line[8];
		int d = rng.below(5);
		for (int i = 0; i < 8; i++) {
			d += rng.below(5) - 2;
			d = std::max(-3, std::min(7, d));
			line[i] = d;
		}
		static const int RH[] = {0, 3, 6, 10, 16, 19, 22, 26};
		for (int b = 0; b < BARS; b += 2)
			for (int i = 0; i < 8; i++) {
				int s = b * STEPS + RH[i];
				if (s >= LEN)
					continue;
				int nextS = i + 1 < 8 ? b * STEPS + RH[i + 1] : (b + 2) * STEPS;
				Chord c = chordAt(p, si, pads, s);
				int pitch = set.key.root + scaleStep(scale, line[i]) + 12 * ((center - set.key.root) / 12);
				if ((i == 0 || i == 4) && c.valid())
					pitch = nearest(c.root + QUALITY_TONES[c.quality][rng.below(3)], pitch);
				if (b == 2 && i >= 6)
					pitch = snapScale(pitch + (rng.chance(0.5f) ? 2 : -3), scale, set.key.root);
				addNote(p.mel, s, pitch, (float) (nextS - s) - 0.1f, 0.8f, i % 2 ? 0.25f : 0.f);
				p.mel[s].slide = i > 0 && rng.chance(0.35f);
			}
	}
	else if (mode == 2) { // ARP: bells through the chord tones, up an octave
		int stepLen = rng.chance(0.5f) ? 2 : 1;
		int dir = rng.chance(0.5f) ? 1 : -1;
		for (int s = 0; s < LEN; s += stepLen) {
			Chord c = chordAt(p, si, pads, s);
			int tones[5];
			int nt = chordTones(c, tones);
			if (!nt)
				continue;
			int k = (s / stepLen) % (nt + 1);
			int idx = dir > 0 ? k : nt - k;
			int pitch = nearest(tones[idx % nt], center + 5) + (idx >= nt ? 12 : 0);
			float prio = (s % 4 == 0) ? 0.f : (stepLen == 1 ? 0.6f : 0.3f);
			addNote(p.mel, s, pitch, stepLen * 0.8f, 0.7f, prio);
		}
	}
	else { // RIFF: a short motif of 4-6 notes, repeated, bar 4 answers
		int n = 4 + rng.below(3);
		int pos[6], deg[6];
		for (int i = 0; i < n; i++) {
			pos[i] = rng.below(STEPS);
			deg[i] = rng.below(6) - 1;
		}
		std::sort(pos, pos + n);
		for (int b = 0; b < BARS; b++)
			for (int i = 0; i < n; i++) {
				int s = b * STEPS + pos[i];
				int dd = deg[i];
				if (b == 3 && i >= n - 2)
					dd += rng.chance(0.5f) ? 2 : -2;
				int pitch = set.key.root + scaleStep(scale, dd) + 12 * ((center - set.key.root) / 12);
				Chord c = chordAt(p, si, pads, s);
				if (jazz && c.valid() && rng.chance(0.5f))
					pitch = nearest(c.root + QUALITY_TONES[c.quality][1 + rng.below(2)], pitch);
				if (jazz && i + 1 < n && pos[i + 1] - pos[i] >= 2 && rng.chance(0.3f))
					addNote(p.mel, s + 1, pitch + 1, 0.8f, 0.5f, 0.6f); // a chromatic step up
				addNote(p.mel, s, pitch, std::max(0.8f, (float) ((i + 1 < n ? pos[i + 1] : STEPS) - pos[i]) * 0.7f), 0.8f,
				        i < 3 ? 0.f : 0.35f);
			}
	}
	// with a sample: where a chop starts, the melody steps back (comes only with more DENS)
	if (si.count > 0)
		for (int s = 0; s < LEN; s++)
			if (p.mel[s].pitch >= 0 && p.chop[s].pad >= 0)
				p.mel[s].prio = std::min(1.f, p.mel[s].prio + 0.4f);
	(void) jazz;
}

// ---------------------------------------------------------------- pads
static void voicing(const Chord& c, int8_t* out) {
	int tones[5];
	int n = chordTones(c, tones);
	for (int i = 0; i < PAD_VOICES; i++)
		out[i] = -1;
	if (!n)
		return;
	int base = 55;
	for (int i = 0; i < PAD_VOICES; i++) {
		int pc = i < n ? tones[i] : tones[0] + 12;
		int note = nearest(pc, base + i * 4);
		out[i] = (int8_t) note;
	}
	std::sort(out, out + PAD_VOICES);
}

static void genPad(Pattern& p, const Style& st, const Settings& set, const SampleInfo& si, const PadSet* pads, Rng& rng) {
	for (int s = 0; s < LEN; s++)
		p.pad[s] = PadEv();
	int mode = set.padMode;
	for (int b = 0; b < BARS; b++) {
		int s0 = b * STEPS;
		if (mode == 1) { // STAB
			static const char* RH[] = {"x.....x...x.....", "x..x......x..x..", "..x...x...x....."};
			const char* rh = RH[rng.below(3)];
			for (int i = 0; i < STEPS; i++)
				if (rh[i] == 'x') {
					PadEv& e = p.pad[s0 + i];
					voicing(chordAt(p, si, pads, s0 + i), e.pitch);
					e.len = 1.5f, e.vel = 0.75f, e.prio = i < 8 ? 0.f : 0.3f;
				}
			continue;
		}
		// SUSTAIN / SWELL: a bar long, or half a bar when the chop's chord changes in the middle
		Chord a = chordAt(p, si, pads, s0);
		Chord m = chordAt(p, si, pads, s0 + 8);
		bool split = m.valid() && a.valid() && (m.root != a.root || m.quality != a.quality);
		PadEv& e = p.pad[s0];
		voicing(a.valid() ? a : p.bar[b], e.pitch);
		e.len = split ? 7.8f : 15.8f;
		e.vel = mode == 2 ? 0.5f : 0.65f;
		e.mod = mode == 2 ? 1.f : 0.f;
		if (split) {
			PadEv& f = p.pad[s0 + 8];
			voicing(m, f.pitch);
			f.len = 7.8f, f.vel = e.vel, f.mod = e.mod;
		}
	}
	(void) st;
}

// ---------------------------------------------------------------- the chops (2.7.0: kinds of pattern, rhythms by style)
const char* const CHOP_PAT_NAMES[NUM_CHOP_PATS] = {"AUTO", "ORDER", "STABS", "CALL+RESP", "HOLD", "MELODY", "HALF ORIG", "SPARSE"};
const char* const CHOP_PAT_TEXT[NUM_CHOP_PATS] = {
	"AUTO (the style chooses)",
	"ORDER (the chops as in the sample)",
	"STABS (short hits on a rhythm)",
	"CALL+RESP (bars 1, 3 call, bars 2, 4 answer)",
	"HOLD (a few long chops, a chord per bar)",
	"MELODY (one chop played up and down the key)",
	"HALF ORIG (one bar as in the sample, one turned round)",
	"SPARSE (one or two chops a bar, room for the voice)",
};

// the styles by family: their own chop rhythms and which kinds AUTO likes
enum Family { F_BOOMBAP, F_JAZZ, F_SOUL, F_DILLA, F_LOFI, F_FUNK, F_ELECTRO, F_TRAP, F_DRILL, NUM_FAMILIES };
static int familyOf(int style) {
	// by the order of the 29 styles in Styles.cpp
	static const int FAM[NUM_STYLES] = {
		F_ELECTRO, F_BOOMBAP, F_BOOMBAP, F_ELECTRO, F_FUNK,      // 80s: Electro, Old School, Golden Age, Miami, New Jack
		F_BOOMBAP, F_JAZZ, F_BOOMBAP, F_FUNK, F_FUNK, F_TRAP, F_TRAP, // 90s: East Coast, Jazz Rap, Dark NY, G-Funk, West Coast, Memphis, Dirty South
		F_SOUL, F_DILLA, F_FUNK, F_ELECTRO, F_JAZZ, F_ELECTRO,   // 2000s: Soul Chop, Dilla, Neptunes, Crunk, Abstract, Snap
		F_TRAP, F_DRILL, F_DRILL, F_TRAP, F_LOFI, F_ELECTRO, F_BOOMBAP, // 2010s: Trap, Chicago Drill, UK Drill, Cloud, Lo-Fi, Ratchet, Grimy
		F_LOFI, F_TRAP, F_TRAP, F_TRAP,                          // 2020s: Drumless, Rage, Plugg, Drift Phonk
	};
	return FAM[std::max(0, std::min(NUM_STYLES - 1, style))];
}
// four or five one-bar rhythms per family (x: a chop starts)
static const char* const RHYTHMS[NUM_FAMILIES][5] = {
	{"x..x..x...x.x...", "x.....x...x..x..", "..x...x..x....x.", "x..x....x..x....", "x...x..x.x...x.."}, // boom bap: off the beat
	{"x.....x.....x...", "x..x.....x..x...", "...x..x....x..x.", "x.......x..x.x..", "x.....x..x......"}, // jazz: lazy, wide
	{"x.x..x.xx.x..x.x", "x..xx..x.x.xx...", "x.x.x..x..x.x.x.", "xx..x.x.xx..x.x.", "x.xx.x..x.xx.x.."}, // soul chop: quick changes
	{"x..x.x....x..x..", ".x..x..x.x...x..", "x...x.x..x.x....", "x..x..x.x.....x.", "..x.x...x..x.x.."}, // dilla: in between
	{"x.......x.......", "x.....x.........", "x.........x.....", "x...........x...", "x.......x...x..."}, // lo-fi: calm
	{"x..x..x.x.x..x.x", "x.xx..x...x.xx..", "..x.x..xx..x..x.", "x..x.x..x.xx...x", "x.x..x.x..x.x..x"}, // funk: 16th syncopation
	{"x.x.x.x.x.x.x.x.", "x..x..x..x..x.x.", "xx..xx..xx..x.x.", "x...x.x...x.x.x.", "x..x..x.x..x..x."}, // electro / crunk: straight
	{"x.......x.....x.", "x.........x.....", "x.....x.......x.", "x..........x..x.", "x.......x......."}, // trap: sparse, long
	{"x..x..x...x..x..", "x.....x..x....x.", "..x..x..x.....x.", "x..x....x..x..x.", "x..x..x.....x..."}, // drill: 3-3-2
};
// how much AUTO likes each kind (ORDER .. SPARSE); ORDER also grows with the style's own chopLoop
static const float KIND_W[NUM_FAMILIES][NUM_CHOP_PATS - 1] = {
	{1.f, 4.f, 3.f, 1.f, 1.f, 2.f, 1.f}, // boom bap
	{2.f, 2.f, 2.f, 3.f, 1.f, 2.f, 1.f}, // jazz
	{1.f, 4.f, 3.f, .5f, 3.f, 2.f, .5f}, // soul chop
	{1.f, 3.f, 2.f, 1.f, 2.f, 3.f, 1.f}, // dilla
	{2.f, 1.f, 1.f, 4.f, 1.f, 2.f, 3.f}, // lo-fi
	{1.f, 4.f, 3.f, 1.f, 2.f, 1.f, 1.f}, // funk
	{.5f, 4.f, 2.f, 1.f, 3.f, 1.f, 2.f}, // electro
	{.5f, 1.f, 1.f, 4.f, 3.f, 1.f, 4.f}, // trap
	{.5f, 2.f, 2.f, 3.f, 3.f, 1.f, 3.f}, // drill
};

int autoChopPat(int style, float r) {
	int f = familyOf(style);
	const Style& st = STYLES[std::max(0, std::min(NUM_STYLES - 1, style))];
	float w[NUM_CHOP_PATS - 1], sum = 0.f;
	for (int k = 0; k < NUM_CHOP_PATS - 1; k++) {
		w[k] = KIND_W[f][k] + (k == 0 ? 3.f * st.chopLoop : 0.f);
		sum += w[k];
	}
	float x = r * sum;
	for (int k = 0; k < NUM_CHOP_PATS - 1; k++) {
		if (x < w[k])
			return k + 1;
		x -= w[k];
	}
	return CP_STABS;
}

static float chopSteps(const SampleInfo& si, int pad, float bpm) {
	float stepSec = 60.f / std::max(30.f, bpm) / 4.f;
	return si.chop[pad].seconds / stepSec;
}

static void putChop(Pattern& p, int s, int pad, float len, int pitch, float prio) {
	if (s < 0 || s >= LEN)
		return;
	ChopEv& e = p.chop[s];
	e = ChopEv();
	e.pad = (int8_t) pad;
	e.len = std::max(0.5f, len);
	e.pitch = (int8_t) pitch;
	e.prio = prio;
}

/** How well pad `pad` (pitched by `semis`) fits chord `want`: 1 the same root, less for a tone of it, a little for a
 *  hit without a chord. */
static float fitOf(const SampleInfo& si, const PadSet* pads, int pad, int semis, const Chord& want) {
	Chord c = padChord(si, pads, pad);
	if (!c.valid())
		return si.chop[pad].kind == 2 ? 0.35f : 0.25f;
	int root = pcOf(c.root + semis + (pads ? pads[pad].pitch : 0));
	bool minor = c.quality == Q_MIN || c.quality == Q_MIN7 || c.quality == Q_MIN9 || c.quality == Q_DIM;
	bool wantMinor = want.quality == Q_MIN || want.quality == Q_MIN7 || want.quality == Q_MIN9 || want.quality == Q_DIM;
	float conf = 0.6f + 0.4f * std::min(1.f, si.chop[pad].conf * 2.f);
	if (root == pcOf(want.root))
		return conf * (minor == wantMinor ? 1.f : 0.8f);
	int t[5];
	int n = chordTones(want, t);
	for (int i = 0; i < n; i++)
		if (pcOf(t[i]) == root)
			return conf * 0.6f;
	return 0.1f;
}

/** The semitones (-6..+5) that move pad's chord root onto `root` (0 for a hit). */
static int pitchTo(const SampleInfo& si, const PadSet* pads, int pad, int root) {
	Chord c = padChord(si, pads, pad);
	if (!c.valid())
		return 0;
	int d = pcOf(root - (c.root + (pads ? pads[pad].pitch : 0)));
	return d > 5 ? d - 12 : d;
}

/** The pads for a bar: the ones that fit its chord best (stabs with a clear chord before bare hits, 2.7.0), `n` of
 *  them, a little shuffled; pitched onto the chord when the style pitches. */
static int padsFor(const SampleInfo& si, const PadSet* pads, const Chord& want, int n, bool preferShort, bool mayPitch, Rng& rng,
                   int* out, int* semis, int avoid = -1) {
	struct C {
		int pad, semis;
		float score;
	};
	C c[PADS];
	int nc = 0;
	for (int i = 0; i < si.count; i++) {
		if (i == avoid && si.count > 2)
			continue;
		float s0 = fitOf(si, pads, i, 0, want);
		int sm = 0;
		if (mayPitch) {
			int d = pitchTo(si, pads, i, want.root);
			float s1 = fitOf(si, pads, i, d, want) * 0.85f; // pitched a little less natural
			if (s1 > s0)
				s0 = s1, sm = d;
		}
		// the kind: stabs (short, with a chord) for STABS, sustained ones for HOLD; very short fragments last
		const ChopInfo& ci = si.chop[i];
		float k = 0.f;
		if (preferShort)
			k = ci.kind == 1 ? 0.25f : (ci.kind == 0 ? 0.1f : 0.f);
		else
			k = ci.kind == 0 ? 0.25f : (ci.kind == 1 ? 0.1f : 0.f);
		if (ci.seconds < 0.06f)
			k -= 0.4f;
		c[nc++] = {i, sm, s0 + k + rng.range(0.f, 0.15f)};
	}
	std::sort(c, c + nc, [](const C& a, const C& b) { return a.score > b.score; });
	int m = std::min(n, nc);
	for (int i = 0; i < m; i++) {
		out[i] = c[i].pad;
		semis[i] = c[i].semis;
	}
	return m;
}

/** The chords the style wants per bar (its progression in the key): the chops follow them (2.7.0). */
static void targetChords(const Style& st, const Settings& set, Chord* out) {
	int scale = scaleFor(st, set.key);
	int deg[BARS] = {1, 1, 1, 1};
	sscanf(st.prog, "%d %d %d %d", &deg[0], &deg[1], &deg[2], &deg[3]);
	for (int b = 0; b < BARS; b++)
		out[b] = degreeChord(scale, set.key.root, deg[b], st.color);
}

/** ORDER: the chops as they sit in the sample, each as long as it is (no cut-short pieces), over one or two bars. */
static void orderBars(Pattern& p, const SampleInfo& si, float bpm, int startPad, int fromBar, int nBars, bool loose, Rng& rng) {
	int pos = 0, i = startPad, span = 0;
	ChopEv A[2 * STEPS];
	for (int k = 0; k < 2 * STEPS; k++)
		A[k] = ChopEv();
	while (pos < 2 * STEPS) {
		float d = chopSteps(si, i, bpm);
		int len = std::max(1, (int) std::lround(d));
		if (loose && len > 1 && rng.chance(0.2f))
			len = std::max(1, len / 2); // only the loose styles (Dilla & co) cut a piece short now and then
		A[pos].pad = (int8_t) i;
		A[pos].len = (float) std::min(len, 2 * STEPS - pos);
		pos += len;
		span = pos;
		i = (i + 1) % si.count;
		if (i == startPad && pos >= STEPS)
			break;
	}
	int alen = span > STEPS ? 2 * STEPS : STEPS;
	for (int s = fromBar * STEPS; s < (fromBar + nBars) * STEPS && s < LEN; s++) {
		const ChopEv& e = A[(s - fromBar * STEPS) % alen];
		if (e.pad >= 0)
			putChop(p, s, e.pad, e.len, 0, 0.f);
	}
}

/** A rhythm's hits on one bar, cycling through `n` pads; every chop lasts until the next one (at most as long as it is). */
static void rhythmBar(Pattern& p, int bar, const char* rh, const int* set, const int* semis, int n, const SampleInfo& si, float bpm,
                      const int* pitchOf, int& k) {
	for (int i = 0; i < STEPS; i++) {
		if (rh[i] != 'x')
			continue;
		int j = k++ % n;
		int pad = set[j];
		int nxt = i + 1;
		while (nxt < STEPS && rh[nxt] != 'x')
			nxt++;
		float len = std::min((float) (nxt - i), std::max(1.f, chopSteps(si, pad, bpm)));
		putChop(p, bar * STEPS + i, pad, len, semis[j] + (pitchOf ? pitchOf[i] : 0), i < 8 ? 0.f : 0.2f);
	}
}

static void genChop(Pattern& p, const Style& st, const Settings& set, const SampleInfo& si, const PadSet* pads, Rng& rng,
                    float bpm) {
	// keep the locked pads' events
	ChopEv kept[LEN];
	bool anyLocked = false;
	for (int s = 0; s < LEN; s++) {
		kept[s] = ChopEv();
		if (p.chop[s].pad >= 0 && pads && pads[p.chop[s].pad].lock)
			kept[s] = p.chop[s], anyLocked = true;
		p.chop[s] = ChopEv();
	}
	if (si.count <= 0)
		return;
	int style = (int) (&st - STYLES);
	int fam = familyOf(style);
	int scale = scaleFor(st, set.key);
	int kind = set.chopPat > CP_AUTO && set.chopPat < NUM_CHOP_PATS ? set.chopPat : autoChopPat(style, rng.uni());
	Chord want[BARS];
	targetChords(st, set, want);
	bool loose = st.chopOff > 0.f;
	bool mayPitch = rng.chance(std::min(1.f, st.chopPitch * 1.6f));
	const char* const* RH = RHYTHMS[fam];
	int startPad = si.count > 4 ? rng.below(std::min(4, si.count)) : 0;
	switch (kind) {
		case CP_ORDER: orderBars(p, si, bpm, startPad, 0, BARS, loose, rng); break;
		case CP_HALF: {
			// bars 1 and 3 as in the sample, bars 2 and 4 the same chops turned round (another order, same rhythm)
			orderBars(p, si, bpm, startPad, 0, BARS, loose, rng);
			for (int b = 1; b < BARS; b += 2) {
				int idx[STEPS], n = 0;
				for (int i = 0; i < STEPS; i++)
					if (p.chop[b * STEPS + i].pad >= 0)
						idx[n++] = b * STEPS + i;
				if (n < 2)
					continue;
				int pd[STEPS];
				for (int i = 0; i < n; i++)
					pd[i] = p.chop[idx[i]].pad;
				int how = rng.below(3);
				for (int i = 0; i < n; i++) {
					int from = how == 0 ? n - 1 - i : (how == 1 ? (i + n / 2) % n : (i % 2 ? i - 1 : std::min(n - 1, i + 1)));
					p.chop[idx[i]].pad = (int8_t) pd[from];
					float d = chopSteps(si, pd[from], bpm);
					int nxt = i + 1 < n ? idx[i + 1] : (b + 1) * STEPS;
					p.chop[idx[i]].len = std::max(0.5f, std::min((float) (nxt - idx[i]), std::max(1.f, d)));
				}
			}
			break;
		}
		case CP_STABS: {
			// 2-4 pads per bar that fit its chord, on one of the family's rhythms (bar 2 sometimes another)
			const char* rh = RH[rng.below(5)];
			const char* rh2 = rng.chance(0.4f) ? RH[rng.below(5)] : rh;
			int pitchOf[STEPS] = {};
			for (int i = 0; i < STEPS; i++)
				if (rh[i] == 'x' && rng.chance(st.chopPitch * 0.5f)) {
					static const int DEG[] = {2, 4, -2, 3, 7, -3};
					pitchOf[i] = scaleStep(scale, DEG[rng.below(6)]);
				}
			int k = 0;
			for (int b = 0; b < BARS; b++) {
				int ps[4], sm[4];
				int n = padsFor(si, pads, want[b], 2 + rng.below(3), true, mayPitch, rng, ps, sm);
				if (n <= 0)
					continue;
				rhythmBar(p, b, b % 2 ? rh2 : rh, ps, sm, n, si, bpm, pitchOf, k);
			}
			break;
		}
		case CP_CALL: {
			// a call (bars 1, 3) and an answer (bars 2, 4) with other pads, mostly in the second half of the bar
			const char* call = RH[rng.below(5)];
			static const char* const RESP[] = {"........x..x.x..", "......x...x.x...", "........x.x...x.", "..........x.x.x.",
			                                   "....x.......x.x."};
			const char* resp = RESP[rng.below(5)];
			int callPad = -1;
			int k = 0;
			for (int b = 0; b < BARS; b++) {
				int ps[3], sm[3];
				bool answer = b % 2 == 1;
				int n = padsFor(si, pads, want[b], answer ? 2 : 3, true, mayPitch, rng, ps, sm, answer ? callPad : -1);
				if (n <= 0)
					continue;
				if (!answer)
					callPad = ps[0];
				rhythmBar(p, b, answer ? resp : call, ps, sm, n, si, bpm, nullptr, k);
			}
			break;
		}
		case CP_HOLD: {
			// one long chop per bar (or half a bar now and then): the chord of the bar
			bool halves = rng.chance(0.35f);
			for (int b = 0; b < BARS; b++) {
				int ps[2], sm[2];
				int n = padsFor(si, pads, want[b], 2, false, true, rng, ps, sm);
				if (n <= 0)
					continue;
				if (halves) {
					putChop(p, b * STEPS, ps[0], 8.f, sm[0], 0.f);
					putChop(p, b * STEPS + 8, ps[n > 1 && rng.chance(0.5f) ? 1 : 0], 8.f, sm[n > 1 ? 1 : 0], 0.15f);
				}
				else
					putChop(p, b * STEPS, ps[0], 16.f, sm[0], 0.f);
			}
			break;
		}
		case CP_MELODY: {
			// one chop with a clear tone, played as a little tune in the key: a motif, bar 4 answers
			int best = 0;
			float bs = -1.f;
			for (int i = 0; i < si.count; i++) {
				const ChopInfo& ci = si.chop[i];
				float s = (ci.chord.valid() ? ci.conf : 0.f) + (ci.kind == 1 ? 0.2f : 0.f) - (ci.seconds < 0.06f ? 1.f : 0.f) +
				          rng.range(0.f, 0.1f);
				if (s > bs)
					bs = s, best = i;
			}
			Chord pc = padChord(si, pads, best);
			int padRoot = pc.valid() ? pc.root + (pads ? pads[best].pitch : 0) : set.key.root;
			const char* rh = RH[rng.below(5)];
			static const int MOTIF[4][6] = {{0, 2, 4, 2, 0, -1}, {0, 4, 2, 5, 4, 2}, {4, 2, 0, -3, 0, 2}, {0, 0, 2, 4, 7, 4}};
			const int* mo = MOTIF[rng.below(4)];
			for (int b = 0; b < BARS; b++) {
				int k = 0;
				int shift = b == 3 ? (rng.chance(0.5f) ? 1 : -2) : 0; // bar 4 answers a step away
				for (int i = 0; i < STEPS; i++) {
					if (rh[i] != 'x')
						continue;
					int deg = mo[k++ % 6] + shift;
					int target = set.key.root + scaleStep(scale, deg);
					int d = target - padRoot;
					while (d > 7)
						d -= 12;
					while (d < -9)
						d += 12;
					int nxt = i + 1;
					while (nxt < STEPS && rh[nxt] != 'x')
						nxt++;
					float len = std::min((float) (nxt - i), std::max(1.f, chopSteps(si, best, bpm)));
					putChop(p, b * STEPS + i, best, len, d, i < 8 ? 0.f : 0.2f);
				}
			}
			break;
		}
		default: { // CP_SPARSE: one or two chops a bar, room for the voice
			static const int AT[NUM_FAMILIES][2] = {{0, 10}, {0, 6}, {0, 8}, {1, 10}, {0, 8}, {0, 11}, {0, 8}, {0, 14}, {0, 9}};
			for (int b = 0; b < BARS; b++) {
				int ps[2], sm[2];
				int n = padsFor(si, pads, want[b], 2, rng.chance(0.5f), mayPitch, rng, ps, sm);
				if (n <= 0)
					continue;
				int a = AT[fam][0], c = AT[fam][1];
				bool two = rng.chance(0.6f);
				float l0 = std::min((float) (two ? c - a : STEPS - a), std::max(1.f, chopSteps(si, ps[0], bpm)));
				putChop(p, b * STEPS + a, ps[0], l0, sm[0], 0.f);
				if (two) {
					int q = n > 1 ? 1 : 0;
					putChop(p, b * STEPS + c, ps[q], std::min((float) (STEPS - c), std::max(1.f, chopSteps(si, ps[q], bpm))), sm[q], 0.3f);
				}
			}
			break;
		}
	}
	// the chipmunk, the drunk timing, now and then backwards
	for (int s = 0; s < LEN; s++) {
		ChopEv& e = p.chop[s];
		if (e.pad < 0)
			continue;
		e.pitch = (int8_t) (e.pitch + st.chopUp);
		e.vel = rng.range(0.8f, 1.f);
		if (st.chopOff > 0.f) {
			e.off = rng.range(-1.f, 1.f) * st.chopOff * 0.35f;
			if (kind != CP_HOLD)
				e.len *= rng.range(0.6f, 1.f);
		}
		if (kind != CP_HOLD && kind != CP_MELODY && rng.chance(st.chopRev * 0.25f))
			e.rev = true;
	}
	// bar 4 answers: a stutter, a reverse or a gap on the last beat (not for HOLD: it rings through)
	int b4 = 3 * STEPS + 12;
	float r = kind == CP_HOLD ? 1.f : rng.uni();
	if (r < st.chopStutter) {
		int s = b4;
		while (s < LEN && p.chop[s].pad < 0)
			s++;
		int pad = s < LEN ? p.chop[s].pad : p.chop[3 * STEPS].pad;
		if (pad >= 0)
			for (int i = 0; i < 4; i++) {
				putChop(p, b4 + i, pad, 0.5f, p.chop[3 * STEPS].pitch, 0.f);
				p.chop[b4 + i].ratchet = (uint8_t) (i >= 2 ? 2 : 1);
			}
	}
	else if (r < st.chopStutter + st.chopRev) {
		for (int i = 0; i < 4; i++)
			if (p.chop[b4 + i].pad >= 0)
				p.chop[b4 + i].rev = true;
	}
	else if (r < st.chopStutter + st.chopRev + 0.2f) {
		for (int i = 2; i < 4; i++)
			p.chop[b4 + i] = ChopEv();
	}
	// the locked pads back in place (new events under them go)
	if (anyLocked)
		for (int s = 0; s < LEN; s++)
			if (kept[s].pad >= 0) {
				int end = std::min(LEN, s + std::max(1, (int) std::ceil(kept[s].len)));
				for (int i = s; i < end; i++)
					p.chop[i] = ChopEv();
				p.chop[s] = kept[s];
			}
}

// ---------------------------------------------------------------- the drums' groove (2.7.0)
const char* const GROOVE_NAMES[NUM_GROOVES] = {"STYLE", "HALF", "DOUBLE", "LAID BACK", "DRUNK", "MINIMAL"};
const char* const GROOVE_TEXT[NUM_GROOVES] = {
	"STYLE (as the style plays)",
	"HALF-TIME (snare on the 3, heavier)",
	"DOUBLE-TIME (kick and snare twice as fast, 16th hats)",
	"LAID BACK (snare and hats a little late)",
	"DRUNK (Dilla: all a little off, the kick stays)",
	"MINIMAL (only kick, snare and hat)",
};

static void applyGroove(Pattern& p, int groove, Rng& rng) {
	if (groove <= G_STYLE || groove >= NUM_GROOVES)
		return;
	for (int b = 0; b < BARS; b++) {
		Hit* bar[DRUMS];
		for (int v = 0; v < DRUMS; v++)
			bar[v] = &p.drum[v][b * STEPS];
		switch (groove) {
			case G_HALF: {
				// the main snares (and claps) of beats 2 and 4 go, one heavy snare on the 3; the kick leaves it room
				bool already = bar[SNARE][8].vel > 0.f && bar[SNARE][4].vel == 0.f && bar[SNARE][12].vel == 0.f;
				if (already)
					break;
				for (int v : {SNARE, CLAP})
					for (int i : {4, 12})
						if (bar[v][i].prio <= 0.1f)
							bar[v][i] = Hit();
				Hit sn;
				sn.vel = 1.f;
				sn.off = rng.range(-0.05f, 0.08f);
				bar[SNARE][8] = sn;
				for (int i = 7; i <= 9; i++)
					if (bar[KICK][i].prio > 0.f || i == 8)
						bar[KICK][i] = Hit();
				// the hats on the eighths
				for (int i = 1; i < STEPS; i += 2)
					if (bar[CH][i].ratchet <= 1)
						bar[CH][i] = Hit();
				break;
			}
			case G_DOUBLE: {
				// the bar's first half played twice as fast for kick, snare and clap; hats on every 16th
				for (int v : {KICK, SNARE, CLAP, RIM}) {
					Hit src[STEPS];
					for (int i = 0; i < STEPS; i++)
						src[i] = bar[v][i];
					for (int i = 0; i < STEPS / 2; i++) {
						bar[v][i] = src[2 * i];
						bar[v][i + STEPS / 2] = src[2 * i];
					}
				}
				for (int i = 0; i < STEPS; i++)
					if (bar[CH][i].vel == 0.f) {
						bar[CH][i].vel = i % 2 ? rng.range(0.3f, 0.45f) : rng.range(0.55f, 0.7f);
						bar[CH][i].prio = i % 2 ? 0.3f : 0.1f;
					}
				break;
			}
			case G_LAID:
				for (int i = 0; i < STEPS; i++) {
					for (int v : {SNARE, CLAP, RIM})
						if (bar[v][i].vel > 0.f)
							bar[v][i].off = std::min(0.45f, std::max(0.f, bar[v][i].off) + rng.range(0.16f, 0.26f));
					for (int v : {CH, OH, PERC})
						if (bar[v][i].vel > 0.f)
							bar[v][i].off = std::min(0.45f, std::max(0.f, bar[v][i].off) + rng.range(0.04f, 0.1f));
				}
				break;
			case G_DRUNK:
				for (int i = 0; i < STEPS; i++)
					for (int v = 0; v < DRUMS; v++) {
						Hit& h = bar[v][i];
						if (h.vel == 0.f)
							continue;
						if (v == KICK)
							h.off = 0.f;
						else if (v == SNARE || v == CLAP)
							h.off = rng.range(0.12f, 0.38f);
						else
							h.off = rng.range(-0.28f, 0.28f);
					}
				break;
			case G_MINIMAL:
				for (int v : {CLAP, RIM, OH, TOM, PERC})
					for (int i = 0; i < STEPS; i++)
						bar[v][i] = Hit();
				for (int i = 0; i < STEPS; i++) {
					if (bar[KICK][i].prio > 0.5f)
						bar[KICK][i] = Hit();
					if (bar[SNARE][i].prio > 0.5f)
						bar[SNARE][i] = Hit();
					if (i % 2 && bar[CH][i].ratchet <= 1)
						bar[CH][i] = Hit();
				}
				break;
		}
	}
}

void generate(Pattern& p, int track, const Settings& set, const SampleInfo& si, const PadSet* pads, uint32_t seed,
              bool lockChords, float bpm) {
	const Style& st = STYLES[std::max(0, std::min(NUM_STYLES - 1, set.style))];
	Rng rng;
	rng.seed(seed * 2654435761u + (uint32_t) (track + 7) * 40503u);
	bool all = track < 0;
	bool harmony = track == T_HARMONY;
	// edited tracks (CHOP GRID) keep their notes, unless their own NEW / GEN asks
	auto may = [&](int t) { return track == t || !p.edited[t]; };
	Chord before[BARS];
	for (int b = 0; b < BARS; b++)
		before[b] = p.bar[b];
	if (track == T_DRUMS || (all && may(T_DRUMS))) {
		genDrums(p, st, rng);
		applyGroove(p, set.drumMode, rng);
	}
	if ((all || track == T_CHOP) && may(T_CHOP))
		genChop(p, st, set, si, pads, rng, bpm);
	if ((all || harmony || track == T_CHOP) && !lockChords)
		genChords(p, st, set, si, pads, rng);
	// the bass leans on the kicks and the chops' chords; the pads on the chords
	// MPC: a lane's own NEW changes only that lane: NEW DRUMS keeps the bass (NEW BASS or NEW ALL roll it again)
	if ((all || harmony || track == T_BASS || track == T_CHOP) && may(T_BASS))
		genBass(p, st, set, si, pads, rng);
	if ((all || harmony || track == T_MEL) && may(T_MEL))
		genMel(p, st, set, si, pads, rng);
	if ((all || harmony || track == T_PAD || track == T_CHOP) && may(T_PAD))
		genPad(p, st, set, si, pads, rng);
	if (track >= 0 && track < NUM_TRACKS) {
		p.edited[track] = false; // generated again: follows the loop, DENS works again
		p.lenTrack[track] = 0;
		if (track == T_DRUMS)
			for (int v = 0; v < DRUMS; v++)
				p.lenDrum[v] = 0;
	}
	// CHORD FOLLOW: edited tone tracks move with the new chords (roots to roots, the rest along the scale)
	if (p.chordFollow) {
		int scale = scaleFor(st, set.key);
		for (int b = 0; b < BARS; b++) {
			int d = pcOf(p.bar[b].root - before[b].root);
			if (d > 6)
				d -= 12;
			if (!d)
				continue;
			for (int s = b * STEPS; s < (b + 1) * STEPS; s++) {
				auto mv = [&](int8_t& n) {
					if (n < 0)
						return;
					bool root = pcOf(n) == pcOf(before[b].root);
					int m = root ? n + d : snapScale(n + d, scale, set.key.root);
					n = (int8_t) std::max(0, std::min(127, m));
				};
				if (p.edited[T_BASS] && track != T_BASS)
					mv(p.bass[s].pitch);
				if (p.edited[T_MEL] && track != T_MEL)
					mv(p.mel[s].pitch);
				if (p.edited[T_PAD] && track != T_PAD)
					for (int k = 0; k < PAD_VOICES; k++)
						mv(p.pad[s].pitch[k]);
			}
		}
	}
}

void mutateChops(Pattern& p, const Settings& set, const SampleInfo& si, const PadSet* pads, uint32_t seed, float amount,
                 float bpm) {
	if (si.count <= 0 || amount <= 0.f)
		return;
	const Style& st = STYLES[std::max(0, std::min(NUM_STYLES - 1, set.style))];
	int scale = scaleFor(st, set.key);
	Rng rng;
	rng.seed(seed * 747796405u + 12345u);
	for (int s = 0; s < LEN; s++) {
		ChopEv& e = p.chop[s];
		if (e.pad < 0 || (pads && pads[e.pad].lock) || !rng.chance(amount * 0.5f))
			continue;
		switch (rng.below(4)) {
			case 0: e.pad = (int8_t) ((e.pad + 1 + rng.below(std::max(1, si.count - 1))) % si.count); break;
			case 1: e.pitch = (int8_t) (st.chopUp + (rng.chance(0.5f) ? scaleStep(scale, rng.below(5) - 1) : 0)); break;
			case 2: e.rev = !e.rev; break;
			default:
				if (e.len >= 2.f)
					e.len *= 0.5f;
				else
					e.ratchet = (uint8_t) (e.ratchet == 1 ? 2 : 1);
				break;
		}
	}
	(void) bpm;
}

// ---------------------------------------------------------------- rendering
static bool cutHas(const Note& n) { return n.pitch >= 0; }
static bool cutHas(const PadEv& e) { return e.pitch[0] >= 0; }
static void cutShorten(Note& n, float len, float vel) {
	if (n.pitch >= 0)
		n.len = std::min(n.len, len), n.vel *= vel;
}
static void cutShorten(PadEv& e, float len, float vel) {
	if (e.pitch[0] >= 0)
		e.len = std::min(e.len, len), e.vel *= vel;
}

template <typename T>
static void cutBeats(T* line, int track, float amount, uint32_t seed) {
	if (amount <= 0.f)
		return;
	T src[LEN];
	for (int s = 0; s < LEN; s++)
		src[s] = line[s];
	for (int q = 0; q < LEN / 4; q++) {
		if (hash01(seed, (uint32_t) track, (uint32_t) q) >= amount * 0.7f)
			continue;
		int op = (int) (hash01(seed + 1, (uint32_t) track, (uint32_t) q) * 4.f);
		int s0 = q * 4;
		switch (op) {
			case 0: // repeat the beat before
				if (q > 0)
					for (int i = 0; i < 4; i++)
						line[s0 + i] = src[s0 - 4 + i];
				break;
			case 1: { // stutter: the beat's first event on every step
				int f = -1;
				for (int i = 0; i < 4 && f < 0; i++)
					if (cutHas(src[s0 + i]))
						f = s0 + i;
				if (f >= 0)
					for (int i = 0; i < 4; i++) {
						line[s0 + i] = src[f];
						cutShorten(line[s0 + i], 0.5f, 1.f - 0.15f * i);
					}
				break;
			}
			case 2: // a gap
				for (int i = 0; i < 4; i++)
					line[s0 + i] = T();
				break;
			default: // the beat backwards
				for (int i = 0; i < 4; i++)
					line[s0 + i] = src[s0 + 3 - i];
				break;
		}
	}
}

static void cutDrums(Hit (*drum)[LEN], float amount, uint32_t seed) {
	if (amount <= 0.f)
		return;
	for (int q = 0; q < LEN / 4; q++) {
		if (hash01(seed, 0u, (uint32_t) q) >= amount * 0.7f)
			continue;
		int op = (int) (hash01(seed + 1, 0u, (uint32_t) q) * 4.f);
		int s0 = q * 4;
		Hit tmp[DRUMS][4];
		for (int v = 0; v < DRUMS; v++)
			for (int i = 0; i < 4; i++)
				tmp[v][i] = drum[v][s0 + i];
		for (int v = 0; v < DRUMS; v++)
			for (int i = 0; i < 4; i++) {
				Hit& h = drum[v][s0 + i];
				switch (op) {
					case 0:
						if (q > 0)
							h = drum[v][s0 - 4 + i];
						break;
					case 1: // the beat's first step stuttered (kick and snare get a roll)
						h = tmp[v][0];
						if (h.vel > 0.f)
							h.vel *= 1.f - 0.18f * i;
						break;
					case 2:
						if (!(v == KICK && i == 0))
							h = Hit();
						break;
					default: h = tmp[v][3 - i]; break;
				}
			}
	}
}

void render(const Pattern& p, const Knobs& k, Render& r) {
	for (int v = 0; v < DRUMS; v++)
		for (int s = 0; s < LEN; s++) {
			Hit h = p.drum[v][s];
			if (h.vel > 0.f && h.prio > k.dens[T_DRUMS] && !p.edited[T_DRUMS])
				h = Hit();
			if (h.ratchet > 1 && k.dens[T_DRUMS] < 0.25f)
				h.ratchet = 1;
			r.drum[v][s] = h;
		}
	for (int s = 0; s < LEN; s++) {
		Note b = p.bass[s];
		if (b.pitch >= 0 && b.prio > k.dens[T_BASS] && !p.edited[T_BASS])
			b = Note();
		if (b.pitch >= 0)
			b.pitch = (int8_t) std::max(0, std::min(127, b.pitch + 12 * k.bassOct));
		r.bass[s] = b;
		Note m = p.mel[s];
		if (m.pitch >= 0 && m.prio > k.dens[T_MEL] && !p.edited[T_MEL])
			m = Note();
		r.mel[s] = m;
		PadEv e = p.pad[s];
		if (e.pitch[0] >= 0 && e.prio > k.dens[T_PAD] && !p.edited[T_PAD])
			e = PadEv();
		r.pad[s] = e;
		ChopEv c = p.chop[s];
		if (c.pad >= 0 && c.prio > k.dens[T_CHOP] && !p.edited[T_CHOP])
			c = ChopEv();
		r.chop[s] = c;
	}
	cutDrums(r.drum, k.cut[T_DRUMS], k.cutSeed);
	cutBeats(r.mel, T_MEL, k.cut[T_MEL], k.cutSeed);
	cutBeats(r.pad, T_PAD, k.cut[T_PAD], k.cutSeed);
}

} // namespace hh
