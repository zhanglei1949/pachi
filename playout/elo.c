/* Playout player based on probability distribution generated over
 * the available moves. */

/* We use the ELO-based (Coulom, 2007) approach, where each board
 * feature (matched pattern, self-atari, capture, MC owner?, ...)
 * is pre-assigned "playing strength" (gamma).
 *
 * Then, the problem of choosing a move is basically a team
 * competition in ELO terms - each spot is represented by a team
 * of features appearing there; the team gamma is product of feature
 * gammas. The team gammas make for a probability distribution of
 * moves to be played.
 *
 * We use the general pattern classifier that will find the features
 * for us, and external datasets that can be harvested from a set
 * of game records (see the HACKING file for details): patterns.spat
 * as a dictionary of spatial stone configurations, and patterns.gamma
 * with strengths of particular features. */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define DEBUG
#include "board.h"
#include "debug.h"
#include "pattern.h"
#include "patternsp.h"
#include "playout.h"
#include "playout/elo.h"
#include "random.h"
#include "tactics.h"
#include "uct/prior.h"

#define PLDEBUGL(n) DEBUGL_(p->debug_level, n)


/* Note that the context can be shared by multiple threads! */

struct patternset {
	pattern_spec ps;
	struct pattern_config pc;
	struct features_gamma *fg;
};

struct elo_policy {
	float selfatari;
	struct patternset choose, assess;
	playout_elo_callbackp callback; void *callback_data;
};


/* This is the core of the policy - initializes and constructs the
 * probability distribution over the move candidates. */

int
elo_get_probdist(struct playout_policy *p, struct patternset *ps, struct board *b, enum stone to_play, struct probdist *pd)
{
	//struct elo_policy *pp = p->data;
	int moves = 0;

	/* First, assign per-point probabilities. */

	for (int f = 0; f < b->flen; f++) {
		struct move m = { .coord = b->f[f], .color = to_play };

		/* Skip pass (for now)? */
		if (is_pass(m.coord)) {
skip_move:
			probdist_set(pd, m.coord, 0);
			continue;
		}
		//fprintf(stderr, "<%d> %s\n", f, coord2sstr(m.coord, b));

		/* Skip invalid moves. */
		if (!board_is_valid_move(b, &m))
			goto skip_move;

		/* We shall never fill our own single-point eyes. */
		/* XXX: In some rare situations, this prunes the best move:
		 * Bulk-five nakade with eye at 1-1 point. */
		if (board_is_one_point_eye(b, m.coord, to_play)) {
			goto skip_move;
		}

		moves++;
		/* Each valid move starts with gamma 1. */
		double g = 1.f;

		/* Some easy features: */
		/* XXX: We just disable them for now since we call the
		 * pattern matcher; you need the gammas file. */
#if 0
		if (is_bad_selfatari(b, to_play, m.coord))
			g *= pp->selfatari;
#endif

		/* Match pattern features: */
		struct pattern p;
		pattern_match(&ps->pc, ps->ps, &p, b, &m);
		for (int i = 0; i < p.n; i++) {
			/* Multiply together gammas of all pattern features. */
			double gamma = feature_gamma(ps->fg, &p.f[i], NULL);
			//char buf[256] = ""; feature2str(buf, &p.f[i]);
			//fprintf(stderr, "<%d> %s feat %s gamma %f\n", f, coord2sstr(m.coord, b), buf, gamma);
			g *= gamma;
		}

		probdist_set(pd, m.coord, g);
		//fprintf(stderr, "<%d> %s %f (E %f)\n", f, coord2sstr(m.coord, b), probdist_one(pd, m.coord), pd->items[f]);
	}

	return moves;
}


struct lprobdist {
	int n;
#define LPD_MAX 8
	coord_t coords[LPD_MAX];
	double items[LPD_MAX];
	double total;
	
	/* Backups of original totals for restoring. */
	double btotal;
	double browtotals_v[10];
	int browtotals_i[10];
	int browtotals_n;
};

#ifdef BOARD_GAMMA

static void
elo_check_probdist(struct playout_policy *p, struct board *b, enum stone to_play, struct probdist *pd, int *ignores, struct lprobdist *lpd, coord_t lc)
{
#if 0
	struct elo_policy *pp = p->data;
	if (pd->total < PROBDIST_EPSILON)
		return;

	/* Compare to the manually created distribution. */
	/* XXX: This is now broken if callback is used. */

	probdist_alloca(pdx, b);
	elo_get_probdist(p, &pp->choose, b, to_play, &pdx);
	for (int i = 0; i < b->flen; i++) {
		coord_t c = b->f[i];
		if (is_pass(c)) continue;
		// XXX: Hardcoded ignores[] structure
		if (ignores[0] == c) continue;
		double val = pd->items[c];
		if (!is_pass(lc) && coord_is_8adjecent(lc, c, b))
			for (int j = 0; j < lpd->n; j++)
				if (lpd->coords[j] == c)
					val = lpd->items[j];
		if (fabs(pdx.items[c] - val) < PROBDIST_EPSILON)
			continue;
		printf("[%s %d] manual %f board %f ", coord2sstr(c, b), b->pat3[c], pdx.items[c], pd->items[c]);
		board_gamma_update(b, c, to_play);
		printf("plainboard %f\n", pd->items[c]);
		assert(0);
	}
#endif
}

coord_t
playout_elo_choose(struct playout_policy *p, struct board *b, enum stone to_play)
{
	struct elo_policy *pp = p->data;
	/* The base board probdist. */
	struct probdist *pd = &b->prob[to_play - 1];
	/* The list of moves we do not consider in pd. */
	int ignores[10]; int ignores_n = 0;
	/* The list of local moves; we consider these separately. */
	struct lprobdist lpd = { .n = 0, .total = 0, .btotal = pd->total, .browtotals_n = 0 };

	/* The engine might want to adjust our probdist. */
	if (pp->callback)
		pp->callback(pp->callback_data, b, to_play, pd);

#define ignore_move(c_) do { \
	ignores[ignores_n++] = c_; \
	if (ignores_n > 1 && ignores[ignores_n - 1] < ignores[ignores_n - 2]) { \
		/* Keep ignores[] sorted. We abuse the fact that we know \
		 * only one item can be out-of-order. */ \
		coord_t cc = ignores[ignores_n - 2]; \
		ignores[ignores_n - 2] = ignores[ignores_n - 1]; \
		ignores[ignores_n - 1] = cc; \
	} \
	int rowi = coord_y(c_, pd->b); \
	lpd.browtotals_i[lpd.browtotals_n] = rowi; \
	lpd.browtotals_v[lpd.browtotals_n++] = pd->rowtotals[rowi]; \
	probdist_mute(pd, c_); \
} while (0)

	/* Make sure ko-prohibited move does not get picked. */
	if (!is_pass(b->ko.coord)) {
		assert(b->ko.color == to_play);
		ignore_move(b->ko.coord);
	}

	/* Contiguity detection. */
	if (!is_pass(b->last_move.coord)) {
		foreach_8neighbor(b, b->last_move.coord) {
			ignore_move(c);

			double val = probdist_one(pd, c) * b->gamma->gamma[FEAT_CONTIGUITY][1];
			lpd.coords[lpd.n] = c;
			lpd.items[lpd.n++] = val;
			lpd.total += val;
		} foreach_8neighbor_end;
	}

	ignores[ignores_n] = pass;

	/* Verify sanity, possibly. */
	elo_check_probdist(p, b, to_play, pd, ignores, &lpd, b->last_move.coord);

	/* Pick a move. */
	coord_t c = pass;
	double stab = fast_frandom() * (lpd.total + pd->total);
	if (stab < lpd.total - PROBDIST_EPSILON) {
		/* Local probdist. */
		for (int i = 0; i < lpd.n; i++) {
			if (stab <= lpd.items[i]) {
				c = lpd.coords[i];
				break;
			}
			stab -= lpd.items[i];
		}
		if (is_pass(c)) {
			fprintf(stderr, "elo: local overstab [%lf]\n", stab);
			abort();
		}

	} else if (pd->total >= PROBDIST_EPSILON) {
		/* Global probdist. */
		/* XXX: We re-stab inside. */
		c = probdist_pick(pd, ignores);

	} else {
		c = pass;
	}

	/* Repair the damage. */
	if (pp->callback) {
		/* XXX: Do something less horribly inefficient
		 * than just recomputing the whole pd. */
		pd->total = 0;
		for (int i = 0; i < board_size(pd->b); i++)
			pd->rowtotals[i] = 0;
		for (int i = 0; i < b->flen; i++) {
			pd->items[b->f[i]] = 0;
			board_gamma_update(b, b->f[i], to_play);
		}
		assert(fabs(pd->total - lpd.btotal) < PROBDIST_EPSILON);

	} else {
		pd->total = lpd.btotal;
		/* If we touched a row multiple times (and we sure will),
		 * the latter value is obsolete; but since we go through
		 * the backups in reverse order, all is good. */
		for (int j = lpd.browtotals_n - 1; j >= 0; j--)
			pd->rowtotals[lpd.browtotals_i[j]] = lpd.browtotals_v[j];
	}
	return c;
}

#else

coord_t
playout_elo_choose(struct playout_policy *p, struct board *b, enum stone to_play)
{
	struct elo_policy *pp = p->data;
	probdist_alloca(pd, b);
	elo_get_probdist(p, &pp->choose, b, to_play, &pd);
	if (pp->callback)
		pp->callback(pp->callback_data, b, to_play, &pd);
	if (pd.total < PROBDIST_EPSILON)
		return pass;
	int ignores[1] = { pass };
	coord_t c = probdist_pick(&pd, ignores);
	return c;
}

#endif

void
playout_elo_assess(struct playout_policy *p, struct prior_map *map, int games)
{
	struct elo_policy *pp = p->data;
	probdist_alloca(pd, map->b);

	int moves;
	moves = elo_get_probdist(p, &pp->assess, map->b, map->to_play, &pd);

	/* It is a question how to transform the gamma to won games; we use
	 * a naive approach currently, but not sure how well it works. */
	/* TODO: Try sqrt(p), atan(p)/pi*2. */

	for (int f = 0; f < map->b->flen; f++) {
		coord_t c = map->b->f[f];
		if (!map->consider[c])
			continue;
		add_prior_value(map, c, probdist_one(&pd, c) / probdist_total(&pd), games);
	}
}

void
playout_elo_done(struct playout_policy *p)
{
	struct elo_policy *pp = p->data;
	features_gamma_done(pp->choose.fg);
	features_gamma_done(pp->assess.fg);
}


void
playout_elo_callback(struct playout_policy *p, playout_elo_callbackp callback, void *data)
{
	struct elo_policy *pp = p->data;
	pp->callback = callback;
	pp->callback_data = data;
}

struct playout_policy *
playout_elo_init(char *arg, struct board *b)
{
	struct playout_policy *p = calloc2(1, sizeof(*p));
	struct elo_policy *pp = calloc2(1, sizeof(*pp));
	p->data = pp;
	p->choose = playout_elo_choose;
	p->assess = playout_elo_assess;
	p->done = playout_elo_done;

	const char *gammafile = features_gamma_filename;
	/* Some defaults based on the table in Remi Coulom's paper. */
	pp->selfatari = 0.06;

	struct pattern_config pc = DEFAULT_PATTERN_CONFIG;
	int xspat = -1;
	bool precise_selfatari = false;

	if (arg) {
		char *optspec, *next = arg;
		while (*next) {
			optspec = next;
			next += strcspn(next, ":");
			if (*next) { *next++ = 0; } else { *next = 0; }

			char *optname = optspec;
			char *optval = strchr(optspec, '=');
			if (optval) *optval++ = 0;

			if (!strcasecmp(optname, "selfatari") && optval) {
				pp->selfatari = atof(optval);
			} else if (!strcasecmp(optname, "precisesa")) {
				/* Use precise self-atari detection within
				 * fast patterns. */
				precise_selfatari = !optval || atoi(optval);
			} else if (!strcasecmp(optname, "gammafile") && optval) {
				/* patterns.gamma by default. We use this,
				 * and need also ${gammafile}f (e.g.
				 * patterns.gammaf) for fast (MC) features. */
				gammafile = strdup(optval);
			} else if (!strcasecmp(optname, "xspat") && optval) {
				/* xspat==0: don't match spatial features
				 * xspat==1: match *only* spatial features */
				xspat = atoi(optval);
			} else {
				fprintf(stderr, "playout-elo: Invalid policy argument %s or missing value\n", optname);
				exit(1);
			}
		}
	}

	pc.spat_dict = spatial_dict_init(false);

	pp->assess.pc = pc;
	pp->assess.fg = features_gamma_init(&pp->assess.pc, gammafile);
	memcpy(pp->assess.ps, PATTERN_SPEC_MATCHALL, sizeof(pattern_spec));
	for (int i = 0; i < FEAT_MAX; i++)
		if ((xspat == 0 && i == FEAT_SPATIAL) || (xspat == 1 && i != FEAT_SPATIAL))
			pp->assess.ps[i] = 0;

	/* In playouts, we need to operate with much smaller set of features
	 * in order to keep reasonable speed. */
	/* TODO: Configurable. */ /* TODO: Tune. */
	pp->choose.pc = FAST_PATTERN_CONFIG;
	pp->choose.pc.spat_dict = pc.spat_dict;
	char cgammafile[256]; strcpy(stpcpy(cgammafile, gammafile), "f");
	pp->choose.fg = features_gamma_init(&pp->choose.pc, cgammafile);
	memcpy(pp->choose.ps, PATTERN_SPEC_MATCHFAST, sizeof(pattern_spec));
	for (int i = 0; i < FEAT_MAX; i++)
		if ((xspat == 0 && i == FEAT_SPATIAL) || (xspat == 1 && i != FEAT_SPATIAL))
			pp->choose.ps[i] = 0;
	if (precise_selfatari)
		pp->choose.ps[FEAT_SELFATARI] = ~(1<<PF_SELFATARI_STUPID);
	board_gamma_set(b, pp->choose.fg, precise_selfatari);

	return p;
}
