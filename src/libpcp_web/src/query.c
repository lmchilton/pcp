/*
 * Copyright (c) 2017-2018 Red Hat.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 */

#include <assert.h>
#include <ctype.h>
#include "util.h"
#include "query.h"
#include "schema.h"
#include "series.h"
#include "libpcp.h"
#include "batons.h"
#include "slots.h"
#include "maps.h"

#define SHA1SZ		20	/* internal sha1 hash buffer size in bytes */
#define QUERY_PHASES	5

typedef struct seriesGetSID {
    seriesBatonMagic	header;		/* MAGIC_SID */
    sds			name;		/* series or source SID */
    sds			metric;		/* back-pointer for instance series */
    void		*baton;
} seriesGetSID;

typedef struct seriesGetLabelMap {
    seriesBatonMagic	header;		/* MAGIC_LABELMAP */
    redisMap		*map;
    sds			series;
    sds			name;
    sds			mapID;
    sds			mapKey;
    void		*baton;
} seriesGetLabelMap;

typedef struct seriesGetLookup {
    redisMap		*map;
    pmSeriesStringCallBack func;
    unsigned int	nseries;
    seriesGetSID	series[0];
} seriesGetLookup;

typedef struct seriesGetQuery {
    node_t		root;
    timing_t		timing;
} seriesGetQuery;

typedef struct seriesQueryBaton {
    seriesBatonMagic	header;		/* MAGIC_QUERY */
    seriesBatonPhase	*current;
    seriesBatonPhase	phases[QUERY_PHASES];
    pmSeriesSettings	*settings;
    void		*userdata;
    redisSlots          *slots;
    int			error;
    union {
	seriesGetLookup	lookup;
	seriesGetQuery	query;
    } u;
} seriesQueryBaton;

static int series_union(series_set_t *, series_set_t *);
static int series_intersect(series_set_t *, series_set_t *);

const char *
series_instance_name(sds key)
{
    size_t	length = sdslen(key);

    if (length >= sizeof("instance.") &&
	strncmp(key, "instance.", sizeof("instance.") - 1) == 0)
	return key + sizeof("instance.") - 1;
    if (length >= sizeof("inst.") &&
	strncmp(key, "inst.", sizeof("inst.") - 1) == 0)
	return key + sizeof("inst.") - 1;
    if (length >= sizeof("i.") &&
	strncmp(key, "i.", sizeof("i.") - 1) == 0)
	return key + sizeof("i.") - 1;
    return NULL;
}

const char *
series_context_name(sds key)
{
    size_t	length = sdslen(key);

    if (length >= sizeof("context.") &&
	strncmp(key, "context.", sizeof("context.") - 1) == 0)
	return key + sizeof("context.") - 1;
    if (length >= sizeof("source.") &&
	strncmp(key, "source.", sizeof("source.") - 1) == 0)
	return key + sizeof("source.") - 1;
    if (length >= sizeof("c.") &&
	strncmp(key, "c.", sizeof("c.") - 1) == 0)
	return key + sizeof("c.") - 1;
    if (length >= sizeof("s.") &&
	strncmp(key, "s.", sizeof("s.") - 1) == 0)
	return key + sizeof("s.") - 1;
    return NULL;
}

const char *
series_metric_name(sds key)
{
    size_t	length = sdslen(key);

    if (length >= sizeof("metric.") &&
	strncmp(key, "metric.", sizeof("metric.") - 1) == 0)
	return key + sizeof("metric.") - 1;
    if (length >= sizeof("m.") &&
	strncmp(key, "m.", sizeof("m.") - 1) == 0)
	return key + sizeof("m.") - 1;
    return NULL;
}

const char *
series_label_name(sds key)
{
    size_t	length = sdslen(key);

    if (length >= sizeof("label.") &&
	strncmp(key, "label.", sizeof("label.") - 1) == 0)
	return key + sizeof("label.") - 1;
    if (length >= sizeof("l.") &&
	strncmp(key, "l.", sizeof("l.") - 1) == 0)
	return key + sizeof("l.") - 1;
    return NULL;
}

const char *
node_subtype(node_t *np)
{
    switch (np->subtype) {
	case N_QUERY: return "query";
	case N_LABEL: return "label";
	case N_METRIC: return "metric";
	case N_CONTEXT: return "context";
	case N_INSTANCE: return "instance";
	default: break;
    }
    return NULL;
}

static int
extract_string(seriesQueryBaton *baton, pmSID series,
		redisReply *reply, sds *string, const char *message)
{
    sds			msg;

    if (reply->type == REDIS_REPLY_STRING) {
	*string = sdscpylen(*string, reply->str, reply->len);
	return 0;
    }
    seriesfmt(msg, "expected string result for %s of series %s (got %s)",
			message, series, redis_reply(reply->type));
    webapimsg(baton, PMLOG_RESPONSE, msg);
    return -EINVAL;
}

static int
extract_mapping(seriesQueryBaton *baton, pmSID series,
		redisReply *reply, sds *string, const char *message)
{
    redisMapEntry	*entry;
    sds			msg;

    if (reply->type == REDIS_REPLY_STRING) {
	if ((entry = redisRMapLookup(baton->u.lookup.map, reply->str)) != NULL) {
	    *string = redisRMapValue(entry);
	    return 0;
	}
	seriesfmt(msg, "bad mapping for %s of series %s", message, series);
	webapimsg(baton, PMLOG_CORRUPT, msg);
	return -EINVAL;
    }
    seriesfmt(msg, "expected string for %s of series %s", message, series);
    webapimsg(baton, PMLOG_RESPONSE, msg);
    return -EPROTO;
}

static int
extract_sha1(seriesQueryBaton *baton, pmSID series,
		redisReply *reply, sds *sha, const char *message)
{
    sds			msg;
    char		*hash;

    if (reply->type != REDIS_REPLY_STRING) {
	seriesfmt(msg, "expected string result for %s of series %s",
			message, series);
	webapimsg(baton, PMLOG_RESPONSE, msg);
	return -EINVAL;
    }
    if (reply->len != 20) {
	seriesfmt(msg, "expected sha1 for %s of series %s, got %ld bytes",
			message, series, (long)reply->len);
	webapimsg(baton, PMLOG_RESPONSE, msg);
	return -EINVAL;
    }
    hash = pmwebapi_hash_str((unsigned char *)reply->str);
    *sha = sdscpylen(*sha, hash, 40);
    return 0;
}

static int
extract_time(seriesQueryBaton *baton, pmSID series,
		redisReply *reply, sds *stamp)
{
    sds			msg, val;
    char		*point;

    if (reply->type == REDIS_REPLY_STATUS) {
	val = sdscpylen(*stamp, reply->str, reply->len);
	if ((point = strchr(val, '-')) != NULL)
	    *point = '.';
	*stamp = val;
	return 0;
    }
    seriesfmt(msg, "expected string timestamp in series %s", series);
    webapimsg(baton, PMLOG_RESPONSE, msg);
    return -EPROTO;
}

/*
 * Report a timeseries result - timestamps and (instance) values
 */
static int
series_instance_reply(seriesQueryBaton *baton, sds series,
	pmSeriesValue *value, int nelements, redisReply **elements)
{
    char		*hash;
    sds			inst;
    int			i, sts = 0;

    for (i = 0; i < nelements; i += 2) {
	inst = value->series;
	if (extract_string(baton, series, elements[i], &inst, "series") < 0) {
	    sts = -EPROTO;
	    continue;
	}
	if (sdslen(inst) == 0) {	/* no InDom, use series */
	    inst = sdscpylen(inst, series, 40);
	} else if (sdslen(inst) == 20) {
	    hash = pmwebapi_hash_str((const unsigned char *)inst);
	    inst = sdscpylen(inst, hash, 40);
	} else {
	    /* TODO: propogate errors and mark records - separate callbacks? */
	    continue;
	}
	value->series = inst;

	if (extract_string(baton, series, elements[i+1], &value->data, "value") < 0)
	    sts = -EPROTO;
	else
	    baton->settings->on_value(series, value, baton->userdata);
    }
    return sts;
}

static int
series_result_reply(seriesQueryBaton *baton, sds series, pmSeriesValue *value,
		int nelements, redisReply **elements)
{
    redisReply		*reply;
    sds			msg;
    int			i, sts;

    /* expecting timestamp:valueset pairs, then instance:value pairs */
    if (nelements % 2) {
	seriesfmt(msg, "expected time:valueset pairs in %s XRANGE", series);
	webapimsg(baton, PMLOG_RESPONSE, msg);
	return -EPROTO;
    }

    for (i = 0; i < nelements; i += 2) {
	reply = elements[i+1];
	if ((sts = extract_time(baton, series, elements[i],
				&value->timestamp)) < 0) {
	    baton->error = sts;
	} else if (reply->type != REDIS_REPLY_ARRAY) {
	    seriesfmt(msg, "expected value array for series %s %s (type=%s)",
			series, XRANGE, redis_reply(reply->type));
	    webapimsg(baton, PMLOG_RESPONSE, msg);
	    baton->error = -EPROTO;
	} else if ((sts = series_instance_reply(baton, series, value,
				reply->elements, reply->element)) < 0) {
	    baton->error = sts;
	}
    }
    return 0;
}

static void
series_values_reply(seriesQueryBaton *baton, sds series,
		int nelements, redisReply **elements, void *arg)
{
    pmSeriesValue	value;
    redisReply		*reply;
    int			i, sts;

    value.timestamp = sdsempty();
    value.series = sdsempty();
    value.data = sdsempty();

    for (i = 0; i < nelements; i++) {
	reply = elements[i];
	if ((sts = series_result_reply(baton, series, &value,
				reply->elements, reply->element)) < 0)
	    baton->error = sts;
    }

    sdsfree(value.timestamp);
    sdsfree(value.series);
    sdsfree(value.data);
}

/*
 * Save the series hash identifiers contained in a Redis response
 * for all series that are not already in this nodes set (union).
 * Used at the leaves of the query tree, then merged result sets
 * are propagated upward.
 */
static int
node_series_reply(seriesQueryBaton *baton, node_t *np, int nelements, redisReply **elements)
{
    series_set_t	set;
    unsigned char	*series;
    redisReply		*reply;
    sds			msg;
    int			i, sts = 0;

    if (nelements <= 0)
	return nelements;

    if ((series = (unsigned char *)calloc(nelements, SHA1SZ)) == NULL) {
	seriesfmt(msg, "out of memory (%s, %" FMT_INT64 " bytes)",
			"series reply", (__int64_t)nelements * SHA1SZ);
	webapimsg(baton, PMLOG_REQUEST, msg);
	return -ENOMEM;
    }
    set.series = series;
    set.nseries = nelements;

    for (i = 0; i < nelements; i++) {
	reply = elements[i];
	if (reply->type == REDIS_REPLY_STRING) {
	    memcpy(series, reply->str, SHA1SZ);
	    if (pmDebugOptions.series)
		printf("    %s\n", pmwebapi_hash_str(series));
	    series += SHA1SZ;
	} else {
	    seriesfmt(msg, "expected string in %s set \"%s\" (type=%s)",
		    node_subtype(np->left), np->left->key,
		    redis_reply(reply->type));
	    webapimsg(baton, PMLOG_REQUEST, msg);
	    sts = -EPROTO;
	}
    }
    if (sts < 0) {
	free(set.series);
	return sts;
    }

    return series_union(&np->result, &set);
}

static int
series_compare(const void *a, const void *b)
{
    return memcmp(a, b, SHA1SZ);
}

/*
 * Form resulting set via intersection of two child sets.
 * Algorithm:
 * - sort the larger set
 * - for each identifier in the smaller set
 *   o bisect to find match in sorted set
 *   o if matching, add it to the current saved set
 *
 * Memory from the smaller set is re-used to hold the result,
 * its memory is trimmed (via realloc) if the final resulting
 * set is smaller, and the larger set is freed on completion.
 */
static int
series_intersect(series_set_t *a, series_set_t *b)
{
    unsigned char	*small, *large, *saved, *cp;
    int			nsmall, nlarge, total, i;

    if (a->nseries >= b->nseries) {
	large = a->series;	nlarge = a->nseries;
	small = b->series;	nsmall = b->nseries;
    } else {
	small = a->series;	nsmall = a->nseries;
	large = b->series;	nlarge = b->nseries;
    }

    if (pmDebugOptions.series)
	printf("Intersect large(%d) and small(%d) series\n", nlarge, nsmall);

    qsort(large, nlarge, SHA1SZ, series_compare);

    for (i = 0, cp = saved = small; i < nsmall; i++, cp ++) {
	if (!bsearch(cp, large, nlarge, SHA1SZ, series_compare))
	    continue;		/* no match, continue advancing cp only */
	if (saved != cp)
	    memcpy(saved, cp, SHA1SZ);
	saved++;		/* stashed, advance cp & saved pointers */
    }

    if ((total = (saved - small)) < nsmall) {
	/* shrink the smaller set down further */
	if ((small = realloc(small, total * SHA1SZ)) == NULL)
	    return -ENOMEM;
    }

    if (pmDebugOptions.series) {
	printf("Intersect result set contains %d series:\n", total);
	for (i = 0, cp = small; i < total; cp++, i++)
	    printf("    %s\n", pmwebapi_hash_str(cp));
    }

    a->nseries = total;
    a->series = small;
    b->series = NULL;
    b->nseries = 0;
    free(large);
    return 0;
}

static int
node_series_intersect(node_t *np, node_t *left, node_t *right)
{
    int			sts;

    if ((sts = series_intersect(&left->result, &right->result)) >= 0)
	np->result = left->result;

    /* finished with child leaves now, results percolated up */
    right->result.nseries = left->result.nseries = 0;
    return sts;
}

/*
 * Form the resulting set from union of two child sets.
 * The larger set is realloc-ated to form the result, if we
 * need to (i.e. if there are entries in the smaller set not
 * in the larger).
 *
 * Iterates over the smaller set doing a binary search of
 * each series identifier, and tracks which ones in the small
 * need to be added to the large set.
 * At the end, add more space to the larger set if needed and
 * append to it.  As a courtesy, since all callers need this,
 * we free the smaller set as well.
 */
static int
series_union(series_set_t *a, series_set_t *b)
{
    unsigned char	*cp, *saved, *large, *small;
    int			nlarge, nsmall, total, need, i;

    if (a->nseries >= b->nseries) {
	large = a->series;	nlarge = a->nseries;
	small = b->series;	nsmall = b->nseries;
    } else {
	small = a->series;	nsmall = a->nseries;
	large = b->series;	nlarge = b->nseries;
    }

    if (pmDebugOptions.series)
	printf("Union of large(%d) and small(%d) series\n", nlarge, nsmall);

    qsort(large, nlarge, SHA1SZ, series_compare);

    for (i = 0, cp = saved = small; i < nsmall; i++, cp += SHA1SZ) {
	if (bsearch(cp, large, nlarge, SHA1SZ, series_compare) != NULL)
	    continue;		/* already present, no need to save */
	if (saved != cp)
	    memcpy(saved, cp, SHA1SZ);
	saved += SHA1SZ;	/* stashed, advance both cp & saved */
    }

    if ((need = (saved - small) / SHA1SZ) > 0) {
	/* grow the larger set to cater for new entries, then add 'em */
	if ((cp = realloc(large, (nlarge + need) * SHA1SZ)) == NULL)
	    return -ENOMEM;
	large = cp;
	cp += (nlarge * SHA1SZ);
	memcpy(cp, small, need * SHA1SZ);
	total = nlarge + need;
    } else {
	total = nlarge;
    }

    if (pmDebugOptions.series) {
	printf("Union result set contains %d series:\n", total);
	for (i = 0, cp = large; i < total; cp += SHA1SZ, i++)
	    printf("    %s\n", pmwebapi_hash_str(cp));
    }

    a->nseries = total;
    a->series = large;
    b->series = NULL;
    b->nseries = 0;
    free(small);
    return 0;
}

static int
node_series_union(node_t *np, node_t *left, node_t *right)
{
    int			sts;

    if ((sts = series_union(&left->result, &right->result)) >= 0)
	np->result = left->result;

    /* finished with child leaves now, results percolated up */
    right->result.nseries = left->result.nseries = 0;
    return sts;
}

/*
 * Add a node subtree representing glob (N_GLOB) pattern matches.
 * Each of these matches are then further evaluated (as if N_EQ).
 * Response format is described at https://redis.io/commands/scan
 */
static int
node_glob_reply(seriesQueryBaton *baton, node_t *np, const char *name, int nelements,
		redisReply **elements)
{
    redisReply		*reply, *r;
    sds			msg, key, *matches;
    unsigned int	i;

    if (nelements != 2) {
	seriesfmt(msg, "expected cursor and results from %s (got %d elements)",
			HSCAN, nelements);
	webapimsg(baton, PMLOG_RESPONSE, msg);
	return -EPROTO;
    }

    /* Update the cursor, in case subsequent calls are needed */
    reply = elements[0];
    if (!reply || reply->type != REDIS_REPLY_STRING) {
	seriesfmt(msg, "expected integer cursor result from %s (got %s)",
			HSCAN, reply ? redis_reply(reply->type) : "null");
	webapimsg(baton, PMLOG_RESPONSE, msg);
	return -EPROTO;
    }
    np->cursor = strtoull(reply->str, NULL, 10);

    reply = elements[1];
    if (!reply || reply->type != REDIS_REPLY_ARRAY) {
	seriesfmt(msg, "expected array of results from %s (got %s)",
			HSCAN, reply ? redis_reply(reply->type) : "null");
	webapimsg(baton, PMLOG_RESPONSE, msg);
	return -EPROTO;
    }

    if ((nelements = reply->elements) == 0) {
	if (np->result.series)
	    free(np->result.series);
	np->result.nseries = 0;
	return 0;
    }

    /* result array sanity checking */
    if (nelements % 2) {
	seriesfmt(msg, "expected even number of results from %s (not %d)",
		    HSCAN, nelements);
	webapimsg(baton, PMLOG_REQUEST, msg);
	return -EPROTO;
    }
    for (i = 0; i < nelements; i += 2) {
	r = reply->element[i];
	if (r->type != REDIS_REPLY_STRING) {
	    seriesfmt(msg, "expected only string results from %s (type=%s)",
		    HSCAN, redis_reply(r->type));
	    webapimsg(baton, PMLOG_REQUEST, msg);
	    return -EPROTO;
	}
    }

    /* response is matching key:value pairs from the scanned hash */
    nelements /= 2;
    if ((matches = (sds *)calloc(nelements, sizeof(sds))) == NULL) {
	seriesfmt(msg, "out of memory (%s, %" FMT_INT64 " bytes)",
			"glob reply", (__int64_t)nelements * sizeof(sds));
	webapimsg(baton, PMLOG_REQUEST, msg);
	return -ENOMEM;
    }
    for (i = 0; i < nelements; i++) {
	r = reply->element[i*2+1];
	key = sdsnew("pcp:series:");
	key = sdscatfmt(key, "%s:%s", name, r->str);
	if (pmDebugOptions.series)
	    printf("adding glob result key: %s\n", key);
	matches[i] = key;
    }
    np->nmatches = nelements;
    np->matches = matches;
    return nelements;
}

static void
series_prepare_maps_glob_reply(redisAsyncContext *c, redisReply *reply, void *arg)
{
    node_t		*np = (node_t *)arg;
    seriesQueryBaton	*baton = (seriesQueryBaton *)np->baton;
    const char		*name;
    node_t		*left;
    sds			msg;

    assert(np->type == N_GLOB);	/* indirect hash lookup with key globbing */

    /* TODO: need to handle multiple sets of results using the cursor. */

    left = np->left;
    name = left->key + sizeof("pcp:map:") - 1;
    if (reply->type != REDIS_REPLY_ARRAY) {
	seriesfmt(msg, "expected array for %s key \"%s\" (type=%s)",
		    node_subtype(left), left->key, redis_reply(reply->type));
	webapimsg(baton, PMLOG_RESPONSE, msg);
	baton->error = -EPROTO;
    } else {
	if (pmDebugOptions.series)
	    printf("%s %s\n", node_subtype(np->left), np->key);
	if (node_glob_reply(baton, np, name, reply->elements, reply->element) < 0)
	    baton->error = -EPROTO;
    }

    seriesPassBaton(&baton->current, baton, "series_prepare_maps_glob_reply");
}

static void
series_prepare_maps_name_reply(redisAsyncContext *c, redisReply *reply, void *arg)
{
    node_t		*np = (node_t *)arg;
    seriesQueryBaton	*baton = (seriesQueryBaton *)np->baton;
    sds			msg;

    assert(np->type == N_NAME);
    assert(np->subtype == N_LABEL || np->subtype == N_CONTEXT);

    if (reply->type != REDIS_REPLY_STRING) {
	seriesfmt(msg, "expected string for %s map \"%s\" (type=%s)",
		node_subtype(np), np->value, redis_reply(reply->type));
	webapimsg(baton, PMLOG_RESPONSE, msg);
	baton->error = -EPROTO;
    } else if (np->subtype == N_LABEL) {
	/* TODO: need to handle JSONB label name nesting. */
	sdsclear(np->key);
	np->key = sdscatprintf(np->key, "pcp:map:%s.%s.value",
				node_subtype(np), reply->str);
    } else if (np->subtype == N_CONTEXT) {
	/* TODO: need lookup via source:context.name set. */
	sdsclear(np->key);
	np->key = sdscatprintf(np->key, "pcp:source:%s.name:%s",
				node_subtype(np), reply->str);
    }

    seriesPassBaton(&baton->current, baton, "series_prepare_maps_name_reply");
}

/*
 * Map human names to internal Redis identifiers.
 */
static int
series_prepare_maps(seriesQueryBaton *baton, node_t *np, int level)
{
    const char		*name;
    sds			cmd, cur, key, val;
    int			sts;

    if (np == NULL)
	return 0;

    if ((sts = series_prepare_maps(baton, np->left, level+1)) < 0)
	return sts;

    if (np->type == N_NAME) {
	/* setup any label name map identifiers needed by direct children */
	if ((name = series_instance_name(np->value)) != NULL) {
	    np->subtype = N_INSTANCE;
	    np->key = sdsnew("pcp:map:inst.name");
	} else if ((name = series_metric_name(np->value)) != NULL) {
	    np->subtype = N_METRIC;
	    np->key = sdsnew("pcp:map:metric.name");
	} else if ((name = series_context_name(np->value)) != NULL) {
	    key = sdsnew("pcp:map:context.name");
	    np->key = sdsdup(key);
	    np->subtype = N_CONTEXT;
	    np->baton = baton;
	    seriesBatonReference(baton, "series_prepare_maps");
	    cmd = redis_command(3);
	    cmd = redis_param_str(cmd, HGET, HGET_LEN);
	    cmd = redis_param_sds(cmd, key);
	    cmd = redis_param_str(cmd, name, strlen(name));
	    redisSlotsRequest(baton->slots, HGET, key, cmd,
				series_prepare_maps_name_reply, np);
	} else {
	    if ((name = series_label_name(np->value)) == NULL)
		name = np->value;
	    key = sdsnew("pcp:map:label.name");
	    np->key = sdsdup(key);
	    np->subtype = N_LABEL;
	    np->baton = baton;
	    seriesBatonReference(baton, "series_prepare_maps");
	    cmd = redis_command(3);
	    cmd = redis_param_str(cmd, HGET, HGET_LEN);
	    cmd = redis_param_sds(cmd, key);
	    cmd = redis_param_str(cmd, name, strlen(name));
	    redisSlotsRequest(baton->slots, HGET, key, cmd,
				series_prepare_maps_name_reply, np);
	}
    } else if (np->type == N_GLOB) {	/* indirect hash lookup with key globbing */
	np->baton = baton;
	seriesBatonReference(baton, "series_prepare_maps");
	cur = sdscatfmt(sdsempty(), "%U", np->cursor);
	val = np->right->value;
	key = sdsdup(np->left->key);
	cmd = redis_command(7);
	cmd = redis_param_str(cmd, HSCAN, HSCAN_LEN);
	cmd = redis_param_sds(cmd, key);
	cmd = redis_param_sds(cmd, cur);	/* cursor */
	cmd = redis_param_str(cmd, "MATCH", sizeof("MATCH")-1);
	cmd = redis_param_sds(cmd, val);	/* pattern */
	cmd = redis_param_str(cmd, "COUNT", sizeof("COUNT")-1);
	cmd = redis_param_str(cmd, "256", sizeof("256")-1);
	sdsfree(cur);
	redisSlotsRequest(baton->slots, HSCAN, key, cmd,
				series_prepare_maps_glob_reply, np);
    }

    return series_prepare_maps(baton, np->right, level+1);
}

static sds
series_node_value(node_t *np)
{
    /* special JSON cases still to do: null, true, false */
    if (np->left->type == N_NAME &&
	np->left->subtype == N_LABEL &&
	np->right->type == N_STRING) {
	np->right->subtype = N_LABEL;
	return sdscatfmt(sdsempty(), "\"%S\"", np->right->value);
    }
    return sdsdup(np->right->value);
}

static void
series_prepare_eval_eq_reply(redisAsyncContext *c, redisReply *reply, void *arg)
{
    node_t		*np = (node_t *)arg;
    seriesQueryBaton	*baton = (seriesQueryBaton *)np->baton;
    const char		*name;
    node_t		*left;
    sds			msg;

    assert(np->type == N_EQ);

    left = np->left;
    name = left->key + sizeof("pcp:map:") - 1;
    if (reply->type == REDIS_REPLY_NIL) {
	seriesfmt(msg, "no match for time series query");
	webapimsg(baton, PMLOG_ERROR, msg);
	baton->error = -EINVAL;
    } else if (reply->type != REDIS_REPLY_STRING) {
	seriesfmt(msg, "expected string for %s key \"%s\" (type=%s)",
		    node_subtype(left), left->key, redis_reply(reply->type));
	webapimsg(baton, PMLOG_RESPONSE, msg);
	baton->error = -EPROTO;
    } else {
	sdsclear(np->key);
	if (np->subtype == N_CONTEXT)
	    np->key = sdscat(np->key, "pcp:source:");
	else
	    np->key = sdscat(np->key, "pcp:series:");
	np->key = sdscatfmt(np->key, "%s:%s", name, reply->str);
    }

    seriesPassBaton(&baton->current, baton, "series_prepare_eval_eq_reply");
}

/*
 * Prepare evaluation of leaf nodes.
 */
static int
series_prepare_eval(seriesQueryBaton *baton, node_t *np, int level)
{
    sds 		key, val, cmd;
    int			sts;

    if (np == NULL)
	return 0;

    if ((sts = series_prepare_eval(baton, np->left, level+1)) < 0)
	return sts;

    switch (np->type) {
    case N_EQ:		/* direct hash lookup */
	val = series_node_value(np);
	key = sdsdup(np->left->key);
	np->baton = baton;
	seriesBatonReference(baton, "series_prepare_eval");
	cmd = redis_command(3);
	cmd = redis_param_str(cmd, HGET, HGET_LEN);
	cmd = redis_param_sds(cmd, key);
	cmd = redis_param_sds(cmd, val);
	sdsfree(val);
	redisSlotsRequest(baton->slots, HGET, key, cmd,
			series_prepare_eval_eq_reply, np);
	break;

    case N_LT:  case N_LEQ: case N_GEQ: case N_GT:  case N_NEQ:
    case N_RNE: case N_REQ: case N_NEG:
    case N_AND: case N_OR:
	/* TODO: additional operators */
	break;

    default:
	break;
    }

    return series_prepare_eval(baton, np->right, level+1);
}

static void
series_prepare_smembers_reply(redisAsyncContext *c, redisReply *reply, void *arg)
{
    node_t		*np = (node_t *)arg;
    seriesQueryBaton	*baton = (seriesQueryBaton *)np->baton;
    sds			msg;
    int			sts;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_prepare_smembers_reply");
    if (reply->type != REDIS_REPLY_ARRAY) {
	seriesfmt(msg, "expected array for %s set \"%s\" (type=%s)",
			node_subtype(np->left), np->right->value,
			redis_reply(reply->type));
	webapimsg(baton, PMLOG_CORRUPT, msg);
	baton->error = -EPROTO;
    } else {
	if (pmDebugOptions.series)
	    printf("%s %s\n", node_subtype(np->left), np->key);
	sts = node_series_reply(baton, np, reply->elements, reply->element);
	if (sts < 0)
	    baton->error = sts;
    }

    if (np->nmatches)
	np->nmatches--;	/* processed one more from this batch */

    seriesPassBaton(&baton->current, baton, "series_prepare_smembers_reply");
}

static void
series_prepare_smembers(seriesQueryBaton *baton, sds kp, node_t *np)
{
    sds                 cmd, key = sdsdup(kp);

    cmd = redis_command(2);
    cmd = redis_param_str(cmd, SMEMBERS, SMEMBERS_LEN);
    cmd = redis_param_sds(cmd, key);
    redisSlotsRequest(baton->slots, SMEMBERS, key, cmd,
			series_prepare_smembers_reply, np);
}

/*
 * Prepare evaluation of internal nodes.
 */
static int
series_prepare_expr(seriesQueryBaton *baton, node_t *np, int level)
{
    int			sts, i;

    if (np == NULL)
	return 0;

    if ((sts = series_prepare_expr(baton, np->left, level+1)) < 0)
	return sts;
    if ((sts = series_prepare_expr(baton, np->right, level+1)) < 0)
	return sts;

    switch (np->type) {
    case N_EQ:		/* direct hash lookup */
	if (np->key) {
	    seriesBatonReference(baton, "series_prepare_expr");
	    np->baton = baton;
	    series_prepare_smembers(baton, np->key, np);
	}
	break;

    case N_GLOB:	/* globbing or regular expression lookups */
    case N_REQ:
    case N_RNE:
	np->baton = baton;
	for (i = 0; i < np->nmatches; i++) {
	    seriesBatonReference(baton, "series_prepare_expr");
	    series_prepare_smembers(baton, np->matches[i], np);
	}
	break;

    case N_LT: case N_LEQ: case N_GEQ: case N_GT: case N_NEQ: case N_NEG:
	/* TODO */
	break;

    case N_AND:
	sts = node_series_intersect(np, np->left, np->right);
	break;

    case N_OR:
	sts = node_series_union(np, np->left, np->right);
	break;

    default:
	break;
    }
    return sts;
}

static void
initSeriesGetQuery(seriesQueryBaton *baton, node_t *root, timing_t *timing)
{
    seriesBatonCheckMagic(baton, MAGIC_QUERY, "initSeriesGetQuery");
    baton->u.query.root = *root;
    baton->u.query.timing = *timing;
}

static void
freeSeriesGetQuery(seriesQueryBaton *baton)
{
    seriesBatonCheckMagic(baton, MAGIC_QUERY, "freeSeriesGetQuery");
    seriesBatonCheckCount(baton, "freeSeriesGetQuery");
    memset(baton, 0, sizeof(seriesQueryBaton));
    free(baton);
}

static void
series_query_finished(void *arg)
{
    seriesQueryBaton	*baton = (seriesQueryBaton *)arg;

    baton->settings->on_done(baton->error, baton->userdata);
    freeSeriesGetQuery(baton);
}

static void
series_query_end_phase(void *arg)
{
    seriesQueryBaton	*baton = (seriesQueryBaton *)arg;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_query_end_phase");

    if (baton->error == 0) {
	seriesPassBaton(&baton->current, baton, "series_query_end_phase");
    } else {	/* fail after waiting on outstanding I/O */
	if (seriesBatonDereference(baton, "series_query_end_phase"))
	    series_query_finished(baton);
    }
}

static void initSeriesGetSID(seriesGetSID *, const char *, void *); /* TODO */
static void freeSeriesGetSID(seriesGetSID *sid);	/* TODO */

static void
series_prepare_time_reply(redisAsyncContext *c, redisReply *reply, void *arg)
{
    seriesGetSID	*sid = (seriesGetSID *)arg;
    seriesQueryBaton	*baton = (seriesQueryBaton *)sid->baton;
    sds			msg;

    seriesBatonCheckMagic(sid, MAGIC_SID, "series_prepare_time_reply");
    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_prepare_time_reply");

    if (reply->type != REDIS_REPLY_ARRAY) {
	seriesfmt(msg, "expected array from %s XSTREAM values (type=%s)",
			sid->name, redis_reply(reply->type));
	webapimsg(baton, PMLOG_RESPONSE, msg);
	baton->error = -EPROTO;
    } else {
	series_values_reply(baton, sid->name, reply->elements, reply->element, arg);
    }
    freeSeriesGetSID(sid);

    series_query_finished(baton);
}

#define DEFAULT_VALUE_COUNT 10

static void
series_prepare_time(seriesQueryBaton *baton, series_set_t *result)
{
    timing_t		*tp = &baton->u.query.timing;
    unsigned char	*series = result->series;
    seriesGetSID	*sid;
    sds			count, start, end, key, cmd;
    unsigned int	i;

    start = sdsnew(timeval_str(&tp->start));
    if (pmDebugOptions.series)
	fprintf(stderr, "START: %s\n", start);

    if (tp->end.tv_sec)
	end = sdsnew(timeval_str(&tp->end));
    else
	end = sdsnew("+");	/* "+" means "no end" - to the most recent */
    if (pmDebugOptions.series)
	fprintf(stderr, "END: %s\n", end);

    if (tp->count == 0)
	tp->count = DEFAULT_VALUE_COUNT;
    count = sdscatfmt(sdsempty(), "%u", tp->count);
    if (pmDebugOptions.series)
	fprintf(stderr, "COUNT: %u\n", tp->count);

    /*
     * Query cache for the time series range (groups of instance:value
     * pairs, with an associated timestamp).
     */
    for (i = 0; i < result->nseries; i++, series += SHA1SZ) {
	sid = calloc(1, sizeof(seriesGetSID));
	initSeriesGetSID(sid, pmwebapi_hash_str(series), baton);
	seriesBatonReference(baton, "series_prepare_time");

	key = sdscatfmt(sdsempty(), "pcp:values:series:%S", sid->name);

	/* XREAD key t1 t2 [COUNT count] */
	cmd = redis_command(6);
	cmd = redis_param_str(cmd, XRANGE, XRANGE_LEN);
	cmd = redis_param_sds(cmd, key);
	cmd = redis_param_sds(cmd, start);
	cmd = redis_param_sds(cmd, end);
	cmd = redis_param_str(cmd, "COUNT", sizeof("COUNT")-1);
	cmd = redis_param_sds(cmd, count);
	redisSlotsRequest(baton->slots, XRANGE, key, cmd,
				series_prepare_time_reply, sid);
    }
    sdsfree(count);
    sdsfree(start);
    sdsfree(end);
}

static void
series_report_set(seriesQueryBaton *baton, series_set_t *set)
{
    unsigned char	*series = set->series;
    sds			sid = NULL;
    int			i;

    if (set->nseries)
	sid = sdsempty();
    for (i = 0; i < set->nseries; series += SHA1SZ, i++) {
	sid = sdscpylen(sid, pmwebapi_hash_str(series), 40);
	baton->settings->on_match(sid, baton->userdata);
    }
    if (sid)
	sdsfree(sid);
    series_query_finished(baton);
}

static void
series_query_report_matches(void *arg)
{
    seriesQueryBaton	*baton = (seriesQueryBaton *)arg;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_query_report_matches");
    series_report_set(baton, &baton->u.query.root.result);
}

static void
series_query_maps(void *arg)
{
    seriesQueryBaton	*baton = (seriesQueryBaton *)arg;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_query_maps");
    series_prepare_maps(baton, &baton->u.query.root, 0);
}

static void
series_query_eval(void *arg)
{
    seriesQueryBaton	*baton = (seriesQueryBaton *)arg;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_query_eval");
    series_prepare_eval(baton, &baton->u.query.root, 0);
}

static void
series_query_expr(void *arg)
{
    seriesQueryBaton	*baton = (seriesQueryBaton *)arg;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_query_expr");
    series_prepare_expr(baton, &baton->u.query.root, 0);
}

static void
series_query_report_values(void *arg)
{
    seriesQueryBaton	*baton = (seriesQueryBaton *)arg;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_query_report_values");
    series_prepare_time(baton, &baton->u.query.root.result);
}

static int
series_time_window(timing_t *tp)
{
    if (tp->ranges || tp->starts || tp->ends || tp->counts)
	return 1;
    return 0;
}

static void
series_query_services(void *arg)
{
    seriesQueryBaton	*baton = (seriesQueryBaton *)arg;
    pmSeriesCommand	*command = &baton->settings->command;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_query_services");

    /* attempt to re-use existing slots connections */
    if (command->slots) {
	baton->slots = command->slots;
	series_query_end_phase(baton);
    } else {
	baton->slots = command->slots =
	    redisSlotsConnect(
		command->hostspec, 1, command->on_info,
		series_query_end_phase, baton->userdata,
		command->events, (void *)baton);
    }
}

static void
initSeriesQueryBaton(seriesQueryBaton *baton,
		pmSeriesSettings *settings, void *userdata)
{
    initSeriesBatonMagic(baton, MAGIC_QUERY);
    baton->settings = settings;
    baton->userdata = userdata;
}

int
series_solve(pmSeriesSettings *settings,
	node_t *root, timing_t *timing, pmflags flags, void *arg)
{
    seriesQueryBaton	*baton;
    unsigned int	i = 0;

    if ((baton = calloc(1, sizeof(seriesQueryBaton))) == NULL)
	return -ENOMEM;
    initSeriesQueryBaton(baton, settings, arg);
    initSeriesGetQuery(baton, root, timing);

    baton->current = &baton->phases[0];
    baton->phases[i++].func = series_query_services;

    /* Resolve label key names (via their map keys) */
    baton->phases[i++].func = series_query_maps;

    /* Resolve sets of series identifiers for leaf nodes */
    baton->phases[i++].func = series_query_eval;

    /* Perform final matching (set of) series solving */
    baton->phases[i++].func = series_query_expr;

    if ((flags & PMFLAG_METADATA) || !series_time_window(timing))
	/* Report matching series IDs, unless time windowing */
	baton->phases[i++].func = series_query_report_matches;
    else
	/* Report actual values within the given time window */
	baton->phases[i++].func = series_query_report_values;

    assert(i <= QUERY_PHASES);
    seriesBatonPhases(baton->current, i, baton);
    return 0;
}

/* build a reverse hash mapping */
static void
reverse_map(seriesQueryBaton *baton, int nkeys, redisReply **elements)
{
    redisReply		*name, *key;
    sds			msg, val;
    unsigned int	i;

    for (i = 0; i < nkeys; i += 2) {
	name = elements[i];
	key = elements[i+1];
	if (name->type == REDIS_REPLY_STRING) {
	    if (key->type == REDIS_REPLY_STRING) {
		val = sdsnewlen(name->str, name->len);
		redisRMapInsert(baton->u.lookup.map, key->str, val);
		sdsfree(val);
	    } else {
		seriesfmt(msg, "expected string key for hashmap (type=%s)",
			redis_reply(key->type));
		webapimsg(baton, PMLOG_RESPONSE, msg);
		baton->error = -EINVAL;
	    }
	} else {
	    seriesfmt(msg, "expected string name for hashmap (type=%s)",
		    redis_reply(name->type));
	    webapimsg(baton, PMLOG_RESPONSE, msg);
	    baton->error = -EINVAL;
	}
    }
}

/*
 * Produce the list of mapped names (requires reverse mapping from IDs)
 */
static int
series_map_reply(seriesQueryBaton *baton, sds series,
		int nelements, redisReply **elements)
{
    redisMapEntry	*entry;
    redisReply		*reply;
    sds			msg;
    unsigned int	i;
    int			sts = 0;

    for (i = 0; i < nelements; i++) {
	reply = elements[i];
	if (reply->type == REDIS_REPLY_STRING) {
	    if ((entry = redisRMapLookup(baton->u.lookup.map, reply->str)) != NULL)
		baton->u.lookup.func(series, redisRMapValue(entry), baton->userdata);
	    else {
		seriesfmt(msg, "%s - timeseries string map", series);
		webapimsg(baton, PMLOG_CORRUPT, msg);
		sts = -EINVAL;
	    }
	} else {
	    seriesfmt(msg, "expected string in %s set (type=%s)",
			series, redis_reply(reply->type));
	    webapimsg(baton, PMLOG_RESPONSE, msg);
	    sts = -EPROTO;
	}
    }

    return sts;
}

static void freeSeriesGetLookup(seriesQueryBaton *baton);  /* TODO */
static void
series_map_keys_callback(redisAsyncContext *c, redisReply *reply, void *arg)
{
    seriesQueryBaton	*baton = (seriesQueryBaton *)arg;
    redisReply		*child;
    sds			val, msg;
    unsigned int	i;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_map_keys_callback");

    if (reply->type == REDIS_REPLY_ARRAY) {
	val = sdsempty();
	for (i = 0; i < reply->elements; i++) {
	    child = reply->element[i];
	    if (child->type == REDIS_REPLY_STRING) {
		    val = sdscpylen(val, child->str, child->len);
		    baton->u.lookup.func(NULL, val, baton->userdata);
	    } else {
		seriesfmt(msg, "bad response for string map %s (%s)",
			HKEYS, redis_reply(child->type));
		webapimsg(baton, PMLOG_RESPONSE, msg);
		sdsfree(val);
		baton->error = -EINVAL;
	    }
	}
	sdsfree(val);
    } else {
	seriesfmt(msg, "expected array from string map %s (reply=%s)",
		HKEYS, redis_reply(reply->type));
	webapimsg(baton, PMLOG_RESPONSE, msg);
    }

    freeSeriesGetLookup(baton);
}

static int
series_map_keys(seriesQueryBaton *baton, const char *name)
{
    sds			cmd, key;

    key = sdscatfmt(sdsempty(), "pcp:map:%s", name);
    cmd = redis_command(2);
    cmd = redis_param_str(cmd, HKEYS, HKEYS_LEN);
    cmd = redis_param_sds(cmd, key);
    redisSlotsRequest(baton->slots, HKEYS, key, cmd,
		   	 series_map_keys_callback, baton);
    return 0;
}

static void
initSeriesGetLabelMap(seriesGetLabelMap *value, sds series, sds name,
		redisMap *map, const char *mapID, sds mapKey, void *baton)
{
    initSeriesBatonMagic(value, MAGIC_LABELMAP);
    value->map = map;
    value->series = series;
    value->name = name;
    value->mapID = sdsnew(mapID);
    value->mapKey = mapKey;
    value->baton = baton;
}

static void
freeSeriesGetLabelMap(seriesGetLabelMap *value)
{
    seriesBatonCheckMagic(value, MAGIC_LABELMAP, "freeSeriesGetLabelMap");

    redisMapRelease(value->map);
    sdsfree(value->mapID);
    sdsfree(value->mapKey);
    memset(value, 0, sizeof(seriesGetLabelMap));
}

static void series_lookup_end_phase(void *arg);	/* TODO */
static void
series_label_value_reply(redisAsyncContext *c, redisReply *reply, void *arg)
{
    seriesGetLabelMap	*value = (seriesGetLabelMap *)arg;
    seriesQueryBaton	*baton = (seriesQueryBaton *)value->baton;
    redisMapEntry	*entry;
    pmSeriesLabel	label;
    sds			msg;

    seriesBatonCheckMagic(value, MAGIC_LABELMAP, "series_label_value_reply");

    /* unpack - produce reverse map of ids-to-values for each entry */
    if (reply->type == REDIS_REPLY_ARRAY)
	reverse_map(baton, reply->elements, reply->element);
    else {
	seriesfmt(msg, "expected array from %s %s.%s.value (type=%s)", HGETALL,
		      "pcp:map:label", value->mapID, redis_reply(reply->type));
	webapimsg(baton, PMLOG_RESPONSE, msg);
	baton->error = -EPROTO;
    }

    if (baton->error == 0) {
	label.name = value->name;
	if ((entry = redisRMapLookup(value->map, value->mapID)) == NULL)
	    label.value = sdsnew("null");
	else
	    label.value = redisRMapValue(entry);

	baton->settings->on_labelmap(value->series, &label, baton->userdata);

	if (entry == NULL)
	    sdsfree(label.value);
    } else {
	seriesfmt(msg, "%s - timeseries name map", value->series);
	webapimsg(baton, PMLOG_CORRUPT, msg);
    }

    freeSeriesGetLabelMap(value);
    series_lookup_end_phase(baton);
}

static int
series_label_reply(seriesQueryBaton *baton, sds series,
		int nelements, redisReply **elements)
{
    seriesGetLabelMap	*labelmap;
    redisMapEntry 	*entry;
    redisReply		*reply;
    redisMap		*vmap;
    sds			msg, key, cmd, name, vkey;
    char		*nmapID, *vmapID;
    unsigned int	i, index;
    int			sts = 0;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_label_reply");

    /* result verification first */
    if (nelements % 2) {
	seriesfmt(msg, "expected even number of results from %s (not %d)",
		    HGETALL, nelements);
	webapimsg(baton, PMLOG_RESPONSE, msg);
	return -EPROTO;
    }
    for (i = 0; i < nelements; i++) {
	reply = elements[i];
	if (reply->type != REDIS_REPLY_STRING) {
	    seriesfmt(msg, "expected only string results from %s (type=%s)",
			HGETALL, redis_reply(reply->type));
	    webapimsg(baton, PMLOG_RESPONSE, msg);
	    return -EPROTO;
	}
    }

    /* perform the label value reverse lookup */
    nelements /= 2;
    for (i = 0; i < nelements; i++) {
	index = i * 2;
	nmapID = elements[index]->str;
	vmapID = elements[index+1]->str;

	if ((entry = redisRMapLookup(baton->u.lookup.map, nmapID)) != NULL) {
	    vkey = sdscatfmt(sdsempty(), "label.%s.value", nmapID);
	    vmap = redisRMapCreate(vkey);
	    name = redisRMapValue(entry);

	    baton->settings->on_label(series, name, baton->userdata);

	    if ((labelmap = calloc(1, sizeof(seriesGetLabelMap))) == NULL) {
		seriesfmt(msg, "%s - label value lookup OOM", series);
		webapimsg(baton, PMLOG_ERROR, msg);
		sts = -ENOMEM;
		continue;
	    }
	    initSeriesGetLabelMap(labelmap, series, name, vmap, vmapID, vkey, baton);

	    key = sdscatfmt(sdsempty(), "pcp:map:label.%s.value", vmapID);
	    cmd = redis_command(2);
	    cmd = redis_param_str(cmd, HGETALL, HGETALL_LEN);
	    cmd = redis_param_sds(cmd, key);
	    redisSlotsRequest(baton->slots, HGETALL, key, cmd,
				series_label_value_reply, labelmap);
	} else {
	    seriesfmt(msg, "%s - timeseries label map", series);
	    webapimsg(baton, PMLOG_CORRUPT, msg);
	    sts = -EINVAL;
	}
    }
    return sts;
}

static void
series_lookup_labels_callback(redisAsyncContext *c, redisReply *reply, void *arg)
{
    seriesGetSID	*sid = (seriesGetSID *)arg;
    seriesQueryBaton    *baton = (seriesQueryBaton *)sid->baton;
    sds			msg;
    int                 sts;

    seriesBatonCheckMagic(sid, MAGIC_SID, "series_lookup_labels_callback");
    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_lookup_labels_callback");

    if (reply->type != REDIS_REPLY_ARRAY) {
	seriesfmt(msg, "expected array from %s %s:%s (type=%s)",
			HGETALL, "pcp:labelvalue:series", sid->name,
			redis_reply(reply->type));
	webapimsg(baton, PMLOG_RESPONSE, msg);
	baton->error = -EPROTO;
    } else if ((sts = series_label_reply(baton, sid->name,
				reply->elements, reply->element)) < 0) {
	baton->error = sts;
    }
    freeSeriesGetSID(sid);

    series_lookup_end_phase(baton);
}

static void
series_lookup_labels(void *arg)
{
    seriesQueryBaton	*baton = (seriesQueryBaton *)arg;
    seriesGetSID	*sid;
    sds			cmd, key;
    unsigned int	i;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_lookup_labels");
    seriesBatonCheckCount(baton, "series_lookup_labels");

    /* unpack - iterate over series and extract labels names and values */
    for (i = 0; i < baton->u.lookup.nseries; i++) {
	seriesBatonReference(baton, "series_lookup_labels");
	sid = &baton->u.lookup.series[i];
	key = sdscatfmt(sdsempty(), "pcp:labelvalue:series:%S", sid->name);
	cmd = redis_command(2);
	cmd = redis_param_str(cmd, HGETALL, HGETALL_LEN);
	cmd = redis_param_sds(cmd, key);
	redisSlotsRequest(baton->slots, HGETALL, key, cmd,
			series_lookup_labels_callback, sid);
    }
}

static void initSeriesGetLookup(seriesQueryBaton *, int, pmSID *,
		pmSeriesStringCallBack, redisMap *);	/* TODO */
static void series_lookup_services(void *);
static void series_lookup_mapping(void *);
static void series_lookup_finished(void *);

int
pmSeriesLabels(pmSeriesSettings *settings, int nseries, pmSID *series, void *arg)
{
    seriesQueryBaton	*baton;
    size_t		bytes;
    unsigned int	i = 0;

    if (nseries < 0)
	return -EINVAL;
    bytes = sizeof(seriesQueryBaton) + (nseries * sizeof(seriesGetSID));
    if ((baton = calloc(1, bytes)) == NULL)
	return -ENOMEM;
    initSeriesQueryBaton(baton, settings, arg);
    initSeriesGetLookup(baton, nseries, series, settings->on_label, labelsrmap);

    if (nseries == 0)
	return series_map_keys(baton, redisMapName(baton->u.lookup.map));

    baton->current = &baton->phases[0];
    baton->phases[i++].func = series_lookup_services;
    baton->phases[i++].func = series_lookup_mapping;
    baton->phases[i++].func = series_lookup_labels;
    baton->phases[i++].func = series_lookup_finished;
    assert(i <= QUERY_PHASES);
    seriesBatonPhases(baton->current, i, baton);
    return 0;
}

static int
extract_series_desc(seriesQueryBaton *baton, pmSID series,
		int nelements, redisReply **elements, pmSeriesDesc *desc)
{
    sds			msg;

    if (nelements < 6) {
	seriesfmt(msg, "bad reply from %s %s (%d)", series, HMGET, nelements);
	webapimsg(baton, PMLOG_RESPONSE, msg);
	return -EPROTO;
    }

    /* sanity check - were we given an invalid series identifier? */
    if (elements[0]->type == REDIS_REPLY_NIL) {
	seriesfmt(msg, "no descriptor for series identifier %s", series);
	webapimsg(baton, PMLOG_ERROR, msg);
	return -EINVAL;
    }

    if (extract_string(baton, series, elements[0], &desc->indom, "indom") < 0)
	return -EPROTO;
    if (extract_string(baton, series, elements[1], &desc->pmid, "pmid") < 0)
	return -EPROTO;
    if (extract_string(baton, series, elements[2], &desc->semantics, "semantics") < 0)
	return -EPROTO;
    if (extract_sha1(baton, series, elements[3], &desc->source, "source") < 0)
	return -EPROTO;
    if (extract_string(baton, series, elements[4], &desc->type, "type") < 0)
	return -EPROTO;
    if (extract_string(baton, series, elements[5], &desc->units, "units") < 0)
	return -EPROTO;

    return 0;
}

static void
redis_series_desc_reply(redisAsyncContext *c, redisReply *reply, void *arg)
{
    pmSeriesDesc	desc;
    seriesGetSID	*sid = (seriesGetSID *)arg;
    seriesQueryBaton	*baton = (seriesQueryBaton *)sid->baton;
    sds			msg;
    int			sts;

    desc.indom = sdsempty();
    desc.pmid = sdsempty();
    desc.semantics = sdsempty();
    desc.source = sdsempty();
    desc.type = sdsempty();
    desc.units = sdsempty();

    if (reply->type != REDIS_REPLY_ARRAY) {
	seriesfmt(msg, "expected array type from series %s %s (type=%s)",
			sid->name, HMGET, redis_reply(reply->type));
	webapimsg(baton, PMLOG_RESPONSE, msg);
	baton->error = -EPROTO;
    }
    else if ((sts = extract_series_desc(baton, sid->name,
			reply->elements, reply->element, &desc)) < 0)
	baton->error = sts;
    else if ((sts = baton->settings->on_desc(sid->name, &desc, baton->userdata)) < 0)
	baton->error = sts;

    sdsfree(desc.indom);
    sdsfree(desc.pmid);
    sdsfree(desc.semantics);
    sdsfree(desc.source);
    sdsfree(desc.type);
    sdsfree(desc.units);

    series_lookup_end_phase(baton);
}

static void
series_lookup_desc(void *arg)
{
    seriesQueryBaton	*baton = (seriesQueryBaton *)arg;
    seriesGetSID	*sid;
    sds			cmd, key;
    unsigned int	i;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_lookup_desc");
    seriesBatonCheckCount(baton, "series_lookup_desc");

    for (i = 0; i < baton->u.lookup.nseries; i++) {
	sid = &baton->u.lookup.series[i];
	seriesBatonReference(baton, "series_lookup_desc");

	key = sdscatfmt(sdsempty(), "pcp:desc:series:%S", sid->name);
	cmd = redis_command(8);
	cmd = redis_param_str(cmd, HMGET, HMGET_LEN);
	cmd = redis_param_sds(cmd, key);
	cmd = redis_param_str(cmd, "indom", sizeof("indom")-1);
	cmd = redis_param_str(cmd, "pmid", sizeof("pmid")-1);
	cmd = redis_param_str(cmd, "semantics", sizeof("semantics")-1);
	cmd = redis_param_str(cmd, "source", sizeof("source")-1);
	cmd = redis_param_str(cmd, "type", sizeof("type")-1);
	cmd = redis_param_str(cmd, "units", sizeof("units")-1);
	redisSlotsRequest(baton->slots, HMGET, key, cmd, redis_series_desc_reply, sid);
    }
}

int
pmSeriesDescs(pmSeriesSettings *settings, int nseries, pmSID *series, void *arg)
{
    seriesQueryBaton	*baton;
    size_t		bytes;
    unsigned int	i = 0;

    if (nseries <= 0)
	return -EINVAL;

    bytes = sizeof(seriesQueryBaton) + (nseries * sizeof(seriesGetSID));
    if ((baton = calloc(1, bytes)) == NULL)
	return -ENOMEM;
    initSeriesQueryBaton(baton, settings, arg);
    initSeriesGetLookup(baton, nseries, series, NULL, NULL);

    baton->current = &baton->phases[0];
    baton->phases[i++].func = series_lookup_services;
    baton->phases[i++].func = series_lookup_desc;
    baton->phases[i++].func = series_lookup_finished;
    assert(i <= QUERY_PHASES);
    seriesBatonPhases(baton->current, i, baton);
    return 0;
}

static int
extract_series_inst(seriesQueryBaton *baton, seriesGetSID *sid,
		pmSeriesInst *inst, int nelements, redisReply **elements)
{
    sds			msg, series = sid->metric;

    if (nelements < 3) {
	seriesfmt(msg, "bad reply from %s %s (%d)", series, HMGET, nelements);
	webapimsg(baton, PMLOG_RESPONSE, msg);
	return -EPROTO;
    }

    if (extract_string(baton, series, elements[0], &inst->instid, "inst") < 0)
	return -EPROTO;
    if (extract_mapping(baton, series, elements[1], &inst->name, "name") < 0)
	return -EPROTO;
    if (extract_sha1(baton, series, elements[2], &inst->series, "series") < 0)
	return -EPROTO;

    /* verify that this instance series matches the given series */
    if (sdscmp(series, inst->series) != 0) {
	seriesfmt(msg, "mismatched series for instance %s of series %s (got %s)",
			sid->name, series, inst->series);
	webapimsg(baton, PMLOG_CORRUPT, msg);
	return -EINVAL;
    }
    /* return instance series identifiers, not the metric series */
    inst->series = sdscpy(inst->series, sid->name);
    return 0;
}

static void
series_instances_reply_callback(redisAsyncContext *c, redisReply *reply, void *arg)
{
    seriesGetSID	*sid = (seriesGetSID *)arg;
    seriesQueryBaton	*baton = (seriesQueryBaton *)sid->baton;
    pmSeriesSettings	*settings = baton->settings;
    pmSeriesInst	inst;
    sds			msg;
    int			sts;

    seriesBatonCheckMagic(sid, MAGIC_SID, "series_instances_reply_callback");
    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_instances_reply_callback");

    inst.instid = sdsempty();
    inst.name = sdsempty();
    inst.series = sdsempty();

    if (reply->type != REDIS_REPLY_ARRAY) {
	seriesfmt(msg, "expected array from series %s %s (type=%s)",
			HMGET, sid->name, redis_reply(reply->type));
	webapimsg(baton, PMLOG_RESPONSE, msg);
	baton->error = -EPROTO;
    }
    else if ((sts = extract_series_inst(baton, sid, &inst,
				reply->elements, reply->element)) < 0)
	baton->error = sts;
    else if ((sts = settings->on_inst(sid->metric, &inst, baton->userdata)) < 0)
	baton->error = sts;
    freeSeriesGetSID(sid);

    sdsfree(inst.instid);
    sdsfree(inst.name);
    sdsfree(inst.series);

    series_lookup_end_phase(baton);
}

static void
series_instances_reply(seriesQueryBaton *baton,
		pmSID series, int nelements, redisReply **elements)
{
    seriesGetSID	*sid;
    pmSID		name = sdsempty();
    sds			key, cmd;
    int			i;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_instances_reply");

    /*
     * Iterate over the instance series identifiers, looking up
     * the instance hash contents for each.
     */
    for (i = 0; i < nelements; i++) {
	if ((sid = calloc(1, sizeof(seriesGetSID))) == NULL) {
	    /* TODO: report error */
	    continue;
	}
	initSeriesGetSID(sid, series, baton);
	sid->metric = sdsempty();

	if (extract_sha1(baton, series, elements[i], &sid->metric, "series") < 0) {
	    /* TODO: report error */
	    continue;
	}
	seriesBatonReference(sid, "series_instances_reply");
	seriesBatonReference(baton, "series_instances_reply");

	key = sdscatfmt(sdsempty(), "pcp:inst:series:%S", sid);
	cmd = redis_command(5);
	cmd = redis_param_str(cmd, HMGET, HMGET_LEN);
	cmd = redis_param_sds(cmd, key);
	cmd = redis_param_str(cmd, "inst", sizeof("inst")-1);
	cmd = redis_param_str(cmd, "name", sizeof("name")-1);
	cmd = redis_param_str(cmd, "series", sizeof("series")-1);
	redisSlotsRequest(baton->slots, HMGET, key, cmd,
				series_instances_reply_callback, sid);
    }
    sdsfree(name);
}

void
series_lookup_instances_callback(redisAsyncContext *c, redisReply *reply, void *arg)
{
    seriesGetSID	*sid = (seriesGetSID *)arg;
    seriesQueryBaton	*baton = (seriesQueryBaton *)sid->baton;
    sds			msg;

    seriesBatonCheckMagic(sid, MAGIC_SID, "series_lookup_instances_callback");
    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_lookup_instances_callback");

    if (reply->type == REDIS_REPLY_ARRAY)
	series_instances_reply(baton, sid->name, reply->elements, reply->element);
    else {
	seriesfmt(msg, "expected array from series %s %s (type=%s)",
			SMEMBERS, sid->name, redis_reply(reply->type));
	webapimsg(baton, PMLOG_RESPONSE, msg);
	baton->error = -EPROTO;
    }

    series_lookup_end_phase(baton);
}

static void
series_lookup_instances(void *arg)
{
    seriesQueryBaton	*baton = (seriesQueryBaton *)arg;
    seriesGetSID	*sid;
    sds			cmd, key;
    int			i;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_lookup_instances_callback");
    seriesBatonCheckCount(baton, "series_lookup_instances_callback");

    for (i = 0; i < baton->u.lookup.nseries; i++) {
	seriesBatonReference(baton, "series_lookup_instances_callback");
	sid = &baton->u.lookup.series[i];
	key = sdscatfmt(sdsempty(), "pcp:instances:series:%S", sid->name);
	cmd = redis_command(2);
	cmd = redis_param_str(cmd, SMEMBERS, SMEMBERS_LEN);
	cmd = redis_param_sds(cmd, key);
	redisSlotsRequest(baton->slots, SMEMBERS, key, cmd,
			series_lookup_instances_callback, sid);
    }
}

int
pmSeriesInstances(pmSeriesSettings *settings, int nseries, pmSID *series, void *arg)
{
    seriesQueryBaton	*baton;
    size_t		bytes;
    unsigned int	i = 0;

    if (nseries < 0)
	return -EINVAL;
    bytes = sizeof(seriesQueryBaton) + (nseries * sizeof(seriesGetSID));
    if ((baton = calloc(1, bytes)) == NULL)
	return -ENOMEM;
    initSeriesQueryBaton(baton, settings, arg);
    initSeriesGetLookup(baton, nseries, series, settings->on_instance, instrmap);

    if (nseries == 0)
	return series_map_keys(baton, redisMapName(baton->u.lookup.map));

    baton->current = &baton->phases[0];
    baton->phases[i++].func = series_lookup_services;
    baton->phases[i++].func = series_lookup_mapping;
    baton->phases[i++].func = series_lookup_instances;
    baton->phases[i++].func = series_lookup_finished;
    assert(i <= QUERY_PHASES);
    seriesBatonPhases(baton->current, i, baton);
    return 0;
}

static void
initSeriesGetSID(seriesGetSID *sid, const char *name, void *baton)
{
    initSeriesBatonMagic(sid, MAGIC_SID);
    sid->name = sdsnew(name);
    sid->baton = baton;
}

static void
freeSeriesGetSID(seriesGetSID *sid)
{
    seriesBatonCheckMagic(sid, MAGIC_SID, "freeSeriesGetSID");
    sdsfree(sid->name);
    memset(sid, 0, sizeof(seriesGetSID));
    free(sid);
}

static void
initSeriesGetLookup(seriesQueryBaton *baton, int nseries, pmSID *series,
		pmSeriesStringCallBack func, redisMap *map)
{
    seriesGetSID	*sid;
    unsigned int	i;

    for (i = 0; i < nseries; i++) {
	sid = &baton->u.lookup.series[i];
	initSeriesGetSID(sid, series[i], baton);
    }
    baton->u.lookup.nseries = nseries;
    baton->u.lookup.func = func;
    baton->u.lookup.map = map;
}

static void
freeSeriesGetLookup(seriesQueryBaton *baton)
{
    seriesGetSID	*sid;
    size_t		bytes;
    unsigned int	i, nseries;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "freeSeriesGetLookup");
    seriesBatonCheckCount(baton, "freeSeriesGetLookup");

    nseries = baton->u.lookup.nseries;
    for (i = 0; i < nseries; i++) {
	sid = &baton->u.lookup.series[i];
	sdsfree(sid->name);
    }
    bytes = sizeof(seriesQueryBaton) + (nseries * sizeof(seriesGetSID));
    memset(baton, 0, bytes);
    free(baton);
}

static void
redis_lookup_mapping_callback(redisAsyncContext *c, redisReply *reply, void *arg)
{
    seriesQueryBaton	*baton = (seriesQueryBaton *)arg;
    sds			msg;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "redis_lookup_mapping_callback");

    /* unpack - produce reverse map of ids-to-names for each context */
    if (reply->type == REDIS_REPLY_ARRAY)
	reverse_map(baton, reply->elements, reply->element);
    else {
	seriesfmt(msg, "expected array from %s %s (type=%s)",
		HGETALL, "pcp:map:context.name", redis_reply(reply->type));
	webapimsg(baton, PMLOG_RESPONSE, msg);
	baton->error = -EPROTO;
    }

    series_lookup_end_phase(baton);
}

static void
series_lookup_mapping(void *arg)
{
    seriesQueryBaton	*baton = (seriesQueryBaton *)arg;
    sds			cmd, key;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_lookup_mapping");
    key = sdscatfmt(sdsempty(), "pcp:map:%s", redisMapName(baton->u.lookup.map));
    cmd = redis_command(2);
    cmd = redis_param_str(cmd, HGETALL, HGETALL_LEN);
    cmd = redis_param_sds(cmd, key);
    redisSlotsRequest(baton->slots, HGETALL, key, cmd,
			redis_lookup_mapping_callback, baton);
}

static void
series_lookup_finished(void *arg)
{
    seriesQueryBaton	*baton = (seriesQueryBaton *)arg;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_lookup_finished");
    baton->settings->on_done(baton->error, baton->userdata);
    freeSeriesGetLookup(baton);
}

static void
series_lookup_end_phase(void *arg)
{
    seriesQueryBaton	*baton = (seriesQueryBaton *)arg;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_lookup_end_phase");

    if (baton->error == 0) {
	seriesPassBaton(&baton->current, baton, "series_lookup_end_phase");
    } else {	/* fail after waiting on outstanding I/O */
	if (seriesBatonDereference(baton, "series_lookup_end_phase"))
	    series_lookup_finished(baton);
    }
}

static void
series_lookup_services(void *arg)
{
    seriesQueryBaton	*baton = (seriesQueryBaton *)arg;
    pmSeriesCommand	*command = &baton->settings->command;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_lookup_services");
    seriesBatonReferences(baton, 1, "series_lookup_services");

    /* attempt to re-use existing slots connections */
    if (command->slots) {
	baton->slots = command->slots;
	series_lookup_end_phase(baton);
    } else {
	baton->slots = command->slots =
	    redisSlotsConnect(
		command->hostspec, 1, command->on_info,
		series_lookup_end_phase, baton->userdata,
		command->events, (void *)baton);
    }
}

static void
redis_get_sid_callback(redisAsyncContext *redis, redisReply *reply, void *arg)
{
    seriesGetSID	*sid = (seriesGetSID *)arg;
    seriesQueryBaton	*baton = (seriesQueryBaton *)sid->baton;
    sds			msg;
    int			sts;

    seriesBatonCheckMagic(sid, MAGIC_SID, "redis_get_sid_callback");
    seriesBatonCheckMagic(baton, MAGIC_QUERY, "redis_get_sid_callback");

    /* unpack - extract names for this source via context name map */
    if (reply->type != REDIS_REPLY_ARRAY) {
	seriesfmt(msg, "expected array from %s %s (type=%s)",
			SMEMBERS, sid->name, redis_reply(reply->type));
	webapimsg(baton, PMLOG_RESPONSE, msg);
	baton->error = -EPROTO;
    } else if ((sts = series_map_reply(baton, sid->name,
			reply->elements, reply->element)) < 0) {
	baton->error = sts;
    }
    series_lookup_end_phase(baton);
}

static void
series_lookup_sources(void *arg)
{
    seriesQueryBaton	*baton = (seriesQueryBaton *)arg;
    seriesGetSID	*sid;
    sds			cmd, key;
    unsigned int	i;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_lookup_sources");
    seriesBatonCheckCount(baton, "series_lookup_sources");

    for (i = 0; i < baton->u.lookup.nseries; i++) {
	seriesBatonReference(baton, "series_lookup_sources");
	sid = &baton->u.lookup.series[i];
	key = sdscatfmt(sdsempty(), "pcp:context.name:source:%S", sid->name);
	cmd = redis_command(2);
	cmd = redis_param_str(cmd, SMEMBERS, SMEMBERS_LEN);
	cmd = redis_param_sds(cmd, key);
	redisSlotsRequest(baton->slots, SMEMBERS, key, cmd,
			redis_get_sid_callback, sid);
    }
}

int
pmSeriesSources(pmSeriesSettings *settings, int nsources, pmSID *sources, void *arg)
{
    seriesQueryBaton	*baton;
    size_t		bytes;
    unsigned int	i = 0;

    if (nsources < 0)
	return -EINVAL;
    bytes = sizeof(seriesQueryBaton) + (nsources * sizeof(seriesGetSID));
    if ((baton = calloc(1, bytes)) == NULL)
	return -ENOMEM;
    initSeriesQueryBaton(baton, settings, arg);
    initSeriesGetLookup(baton, nsources, sources, settings->on_context, contextrmap);

    if (nsources == 0)
	return series_map_keys(baton, redisMapName(baton->u.lookup.map));

    baton->current = &baton->phases[0];
    baton->phases[i++].func = series_lookup_services;
    baton->phases[i++].func = series_lookup_mapping;
    baton->phases[i++].func = series_lookup_sources;
    baton->phases[i++].func = series_lookup_finished;
    assert(i <= QUERY_PHASES);
    seriesBatonPhases(baton->current, i, baton);
    return 0;
}

static void
series_lookup_metrics(void *arg)
{
    seriesQueryBaton	*baton = (seriesQueryBaton *)arg;
    seriesGetSID	*sid;
    sds			cmd, key;
    unsigned int	i;

    seriesBatonCheckMagic(baton, MAGIC_QUERY, "series_lookup_metrics");
    seriesBatonCheckCount(baton, "series_lookup_metrics");

    for (i = 0; i < baton->u.lookup.nseries; i++) {
	seriesBatonReference(baton, "series_lookup_metrics");
	sid = &baton->u.lookup.series[i];
	key = sdscatfmt(sdsempty(), "pcp:metric.name:series:%S", sid->name);
	cmd = redis_command(2);
	cmd = redis_param_str(cmd, SMEMBERS, SMEMBERS_LEN);
	cmd = redis_param_sds(cmd, key);
	redisSlotsRequest(baton->slots, SMEMBERS, key, cmd,
			redis_get_sid_callback, sid);
    }
}

int
pmSeriesMetrics(pmSeriesSettings *settings, int nseries, pmSID *series, void *arg)
{
    seriesQueryBaton	*baton;
    size_t		bytes;
    unsigned int	i = 0;

    if (nseries < 0)
	return -EINVAL;
    bytes = sizeof(seriesQueryBaton) + (nseries * sizeof(seriesGetSID));
    if ((baton = calloc(1, bytes)) == NULL)
	return -ENOMEM;
    initSeriesQueryBaton(baton, settings, arg);
    initSeriesGetLookup(baton, nseries, series, settings->on_metric, namesrmap);

    if (nseries == 0)
	return series_map_keys(baton, redisMapName(baton->u.lookup.map));

    baton->current = &baton->phases[0];
    baton->phases[i++].func = series_lookup_services;
    baton->phases[i++].func = series_lookup_mapping;
    baton->phases[i++].func = series_lookup_metrics;
    baton->phases[i++].func = series_lookup_finished;
    assert(i <= QUERY_PHASES);
    seriesBatonPhases(baton->current, i, baton);
    return 0;
}
