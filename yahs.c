/*********************************************************************************
 * MIT License                                                                   *
 *                                                                               *
 * Copyright (c) 2021 Chenxi Zhou <chnx.zhou@gmail.com>                          *
 *                                                                               *
 * Permission is hereby granted, free of charge, to any person obtaining a copy  *
 * of this software and associated documentation files (the "Software"), to deal *
 * in the Software without restriction, including without limitation the rights  *
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell     *
 * copies of the Software, and to permit persons to whom the Software is         *
 * furnished to do so, subject to the following conditions:                      *
 *                                                                               *
 * The above copyright notice and this permission notice shall be included in    *
 * all copies or substantial portions of the Software.                           *
 *                                                                               *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR    *
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,      *
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE   *
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER        *
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, *
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE *
 * SOFTWARE.                                                                     *
 *********************************************************************************/

/********************************** Revision History *****************************
 *                                                                               *
 * 22/06/21 - Chenxi Zhou: Created                                               *
 *                                                                               *
 *********************************************************************************/
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <ctype.h>

#include "ketopt.h"
#include "kvec.h"
#include "sdict.h"
#include "link.h"
#include "graph.h"
#include "break.h"
#include "enzyme.h"
#include "asset.h"

#define YAHS_VERSION "1.2a.1"

#undef DEBUG_ERROR_BREAK
#undef DEBUG_GRAPH_PRUNE
#undef DEBUG_OPTIONS
#undef DEBUG_RAM_USAGE
#undef DEBUG_QLF
#undef DEBUG_ENZ
#undef DEBUG_GT4G
#undef DEBUG_LINK

#define ENOMEM_ERR 15
#define ENOBND_ERR 14
#define GB 0x40000000
#define MAX_N_SEQ 45000

#ifndef DEBUG_GT4G
static int ec_min_window = 1000000;
static int ec_resolution = 10000;
static int ec_bin = 1000;
static int ec_move_avg = 0;
static int ec_merge_thresh = 10000;
static int ec_dual_break_thresh = 50000;
#else
static int ec_min_window = 5000000;
static int ec_resolution = 50000;
static int ec_bin = 5000;
static int ec_move_avg = 0;
static int ec_merge_thresh = 50000;
static int ec_dual_break_thresh = 250000;
#endif
static double ec_min_frac = .8;
static double ec_fold_thresh = .2;

static uint64_t n_stats[10];
static uint32_t l_stats[10];

double qbinom(double, double, double, int, int);

int VERBOSE = 0;

static double ys_realtime0;

graph_t *build_graph_from_links(inter_link_mat_t *link_mat, asm_dict_t *dict, double min_norm, double la)
{
    int32_t i, j, n, c0, c1;
    int8_t t;
    double norm, qla;
    inter_link_t *link;
    graph_t *g;
    graph_arc_t *arc;

    g = graph_init();
    g->sdict = dict;

    // build graph
    n = link_mat->n;
    for (i = 0; i < n; ++i) {
        link = &link_mat->links[i];
        if (link->n == 0)
            continue;
        c0 = link->c0;
        c1 = link->c1;
        t = link->linkt;
        if (!t)
            continue;
        
        qla = qbinom(.99, link->n0, la, 1, 0) / link->n0;
        for (j = 0; j < 4; ++j) {
            if (1 << j & t) {
                norm = link->norms[j];
                if (norm >= min_norm) {
                    if (norm < qla) {
#ifdef DEBUG_QLF
                        fprintf(stderr, "[DEBUG_QLF::%s] #Edge rejected by QL filter: %s %s %u %u %.3f (< %.3f)\n", __func__, dict->s[c0].name, dict->s[c1].name, j, link->n0, norm, qla);
#endif
                        continue;
                    }
                    arc = graph_add_arc(g, c0<<1|j>>1, c1<<1|(j&1), -1, 0, norm);
                    graph_add_arc(g, c1<<1|!(j&1), c0<<1|!(j>>1), arc->link_id, 0, norm);
                }
            }
        }
    }

    graph_arc_sort(g);
    graph_arc_index(g);

    return g;
}

int run_scaffolding(char *fai, char *agp, char *link_file, uint32_t ml, uint8_t mq, re_cuts_t *re_cuts, char *out, int resolution, double *noise, long rss_limit, int no_mem_check)
{
    //TODO: adjust wt thres by resolution
    sdict_t *sdict = make_sdict_from_index(fai, ml);
    asm_dict_t *dict = agp? make_asm_dict_from_agp(sdict, agp) : make_asm_dict_from_sdict(sdict);
    
    int i;
    uint64_t len = 0;
    for (i = 0; i < dict->n; ++i)
        len += dict->s[i].len;
#ifdef DEBUG_GRAPH_PRUNE
    fprintf(stderr, "[DEBUG_GRAPH_PRUNE::%s] #sequences loaded %d = %lubp\n", __func__, dict->n, len);
#endif

    long rss_intra, rss_inter;

    rss_intra = no_mem_check? 0 : estimate_intra_link_mat_init_rss(dict, resolution);
    if ((rss_limit >= 0 && rss_intra > rss_limit) || rss_intra < 0) {
        // no enough memory
        fprintf(stderr, "[I::%s] No enough memory. Try higher resolutions... End of scaffolding round.\n", __func__);
        fprintf(stderr, "[I::%s] RAM    limit: %.3fGB\n", __func__, (double) rss_limit / GB);
        fprintf(stderr, "[I::%s] RAM required: %.3fGB\n", __func__, (double) rss_intra / GB);
        asm_destroy(dict);
        sd_destroy(sdict);
        return ENOMEM_ERR;
    }
    rss_limit -= rss_intra;
    fprintf(stderr, "[I::%s] starting norm estimation...\n", __func__);
    intra_link_mat_t *intra_link_mat = intra_link_mat_from_file(link_file, dict, re_cuts, resolution, 1, mq);

#ifdef DEBUG_RAM_USAGE
    fprintf(stderr, "[DEBUG_RAM_USAGE::%s] RAM  peak: %.3fGB\n", __func__, (double) peakrss() / GB);
    fprintf(stderr, "[DEBUG_RAM_USAGE::%s] RAM intra: %.3fGB\n", __func__, (double) rss_intra / GB);
    fprintf(stderr, "[DEBUG_RAM_USAGE::%s] RAM  free: %.3fGB\n", __func__, (double) rss_limit / GB);
#endif

    norm_t *norm = calc_norms(intra_link_mat);
    if (norm == 0) {
        fprintf(stderr, "[W::%s] No enough bands for norm calculation... End of scaffolding round.\n", __func__);
        intra_link_mat_destroy(intra_link_mat);
        asm_destroy(dict);
        sd_destroy(sdict);
        return ENOBND_ERR;
    }

    rss_inter = no_mem_check? 0 : estimate_inter_link_mat_init_rss(dict, resolution, norm->r);
    if ((rss_limit >= 0 && rss_inter > rss_limit) || rss_inter < 0) {
        // no enough memory
        fprintf(stderr, "[I::%s] No enough memory. Try higher resolutions... End of scaffolding round.\n", __func__);
        fprintf(stderr, "[I::%s] RAM    limit: %.3fGB\n", __func__, (double) rss_limit / GB);
        fprintf(stderr, "[I::%s] RAM required: %.3fGB\n", __func__, (double) rss_inter / GB);
        asm_destroy(dict);
        sd_destroy(sdict);
        return ENOMEM_ERR;
    }
    rss_limit -= rss_inter;
    fprintf(stderr, "[I::%s] starting link estimation...\n", __func__);
    inter_link_mat_t *inter_link_mat = inter_link_mat_from_file(link_file, dict, re_cuts, resolution, norm->r, mq);

#ifdef DEBUG_RAM_USAGE
    fprintf(stderr, "[DEBUG_RAM_USAGE::%s] RAM  peak: %.3fGB\n", __func__, (double) peakrss() / GB);
    fprintf(stderr, "[DEBUG_RAM_USAGE::%s] RAM inter: %.3fGB\n", __func__, (double) rss_inter / GB);
    fprintf(stderr, "[DEBUG_RAM_USAGE::%s] RAM  free: %.3fGB\n", __func__, (double) rss_limit / GB);
#endif

    *noise = inter_link_mat->noise / resolution / resolution;

    int8_t *directs = 0;
    double la;
    // directs = calc_link_directs_from_file(link_file, dict);
    inter_link_norms(inter_link_mat, norm, 1, &la);
    calc_link_directs(inter_link_mat, .1, dict, directs);
    free(directs);

#ifdef DEBUG_LINK
    fprintf(stderr, "[DEBUG_LINK::%s] print_inter_link_norms\n", __func__);
    print_inter_link_norms(stderr, inter_link_mat, dict);
#endif

    fprintf(stderr, "[I::%s] starting scaffolding graph contruction...\n", __func__);
    graph_t *g = build_graph_from_links(inter_link_mat, dict, .1, la);

#ifdef DEBUG_GRAPH_PRUNE
    fprintf(stderr, "[DEBUG_GRAPH_PRUNE::%s] scaffolding graph (before pruning) in GV format\n", __func__);
    graph_print_gv(g, stderr);
    fprintf(stderr, "[DEBUG_GRAPH_PRUNE::%s] scaffolding graph (before pruning) in GFA format\n", __func__);
    graph_print(g, stderr, 1);
#endif

    uint64_t n_arc;
    n_arc = g->n_arc;
#ifdef DEBUG_GRAPH_PRUNE
    fprintf(stderr, "[DEBUG_GRAPH_PRUNE::%s] number edges before trimming: %ld\n", __func__, n_arc);
    int round = 0;
#endif
    while (1) {
        trim_graph_simple_filter(g, .1, .7, .1, 0);
#ifdef DEBUG_GRAPH_PRUNE
        fprintf(stderr, "[DEBUG_GRAPH_PRUNE::%s] number edges after simple trimming round %d: %ld\n", __func__, round, g->n_arc);
        graph_print_gv(g, stderr);
#endif
        trim_graph_tips(g);
        trim_graph_blunts(g);
        trim_graph_repeats(g);
        trim_graph_transitive_edges(g);
        trim_graph_pop_bubbles(g);
        trim_graph_pop_undirected(g);
        trim_graph_weak_edges(g);
        trim_graph_self_loops(g);
#ifdef DEBUG_GRAPH_PRUNE
        fprintf(stderr, "[DEBUG_GRAPH_PRUNE::%s] number edges after trimming round %d: %ld\n", __func__, ++round, g->n_arc);
        graph_print_gv(g, stderr);
#endif
        if (g->n_arc == n_arc)
            break;
        else
            n_arc = g->n_arc;
    }
    trim_graph_ambiguous_edges(g);

#ifdef DEBUG_GRAPH_PRUNE
    fprintf(stderr, "[DEBUG_GRAPH_PRUNE::%s] scaffolding graph (after pruning) in GV format\n", __func__);
    graph_print_gv(g, stderr);
    fprintf(stderr, "[DEBUG_GRAPH_PRUNE::%s] scaffolding graph (after pruning) in GFA format\n", __func__);
    graph_print(g, stderr, 1);
#endif

    search_graph_path(g, g->sdict, out);

    intra_link_mat_destroy(intra_link_mat);
    inter_link_mat_destroy(inter_link_mat);
    norm_destroy(norm);
    graph_destroy(g);
    asm_destroy(dict);
    sd_destroy(sdict);

    return 0;
}

int contig_error_break(char *fai, char *link_file, uint32_t ml, char *out)
{
    uint32_t i, ec_round, err_no, bp_n;
    sdict_t *sdict;
    asm_dict_t *dict;
    int dist_thres;

    sdict = make_sdict_from_index(fai, ml);
    dict = make_asm_dict_from_sdict(sdict);
    dist_thres = estimate_dist_thres_from_file(link_file, dict, ec_min_frac, ec_resolution, 0);
    dist_thres = MAX(dist_thres, ec_min_window);
    fprintf(stderr, "[I::%s] dist threshold for contig error break: %d\n", __func__, dist_thres);
    asm_destroy(dict);

    char* out1 = (char *) malloc(strlen(out) + 35);
    ec_round = err_no = 0;
    while (1) {
        dict = ec_round? make_asm_dict_from_agp(sdict, out1) : make_asm_dict_from_sdict(sdict);
        link_mat_t *link_mat = link_mat_from_file(link_file, dict, dist_thres, ec_bin, .0, ec_move_avg, 0);
#ifdef DEBUG_ERROR_BREAK
        fprintf(stderr, "[DEBUG_ERROR_BREAK::%s] ec_round %u link matrix\n", __func__, ec_round);
        print_link_mat(link_mat, dict, stderr);
#endif
        bp_n = 0;
        bp_t *breaks = detect_break_points(link_mat, ec_bin, ec_merge_thresh, ec_fold_thresh, ec_dual_break_thresh, &bp_n);
#ifdef DEBUG
        fprintf(stderr, "[DEBUG::%s] number contig breaks in round %u: %u\n", __func__, ec_round + 1, bp_n);
#endif
        sprintf(out1, "%s_%02d.agp", out, ++ec_round);
        FILE *agp_out = fopen(out1, "w");
        write_break_agp(dict, breaks, bp_n, agp_out);
        fclose(agp_out);
        
        link_mat_destroy(link_mat);
        asm_destroy(dict);
        for (i = 0; i < bp_n; ++i)
            free(breaks[i].p);
        free(breaks);
        
        err_no += bp_n;
#ifdef DEBUG_ERROR_BREAK
        fprintf(stderr, "[DEBUG_ERROR_BREAK::%s] bp_n %d\n", __func__, bp_n);
#endif
        if (!bp_n)
            break;
    }
    sd_destroy(sdict);
    free(out1);

    fprintf(stderr, "[I::%s] performed %u round assembly error correction. Made %u breaks \n", __func__, ec_round, err_no);

    return ec_round;
}

int scaffold_error_break(char *fai, char *link_file, uint32_t ml, uint8_t mq, char *agp, int flank_size, double noise, char *out)
{
    int dist_thres;
    sdict_t *sdict = make_sdict_from_index(fai, ml);
    asm_dict_t *dict = make_asm_dict_from_agp(sdict, agp);

    dist_thres = flank_size * 2;
    //dist_thres = estimate_dist_thres_from_file(link_file, dict, ec_min_frac, ec_resolution);
    //dist_thres = MAX(dist_thres, ec_min_window);
    //fprintf(stderr, "[I::%s] dist threshold for scaffold error break: %d\n", __func__, dist_thres);
    link_mat_t *link_mat = link_mat_from_file(link_file, dict, dist_thres, ec_bin, noise, ec_move_avg, mq);

#ifdef DEBUG_ERROR_BREAK
    fprintf(stderr, "[DEBUG_ERROR_BREAK::%s] link matrix\n", __func__);
    print_link_mat(link_mat, dict, stderr);
#endif

    uint32_t bp_n = 0;
    bp_t *breaks = detect_break_points_local_joint(link_mat, ec_bin, ec_fold_thresh, flank_size, dict, &bp_n);
#ifdef DEBUG
    fprintf(stderr, "[DEBUG::%s] number scaffold breaks: %u\n", __func__, bp_n);
#endif

    FILE *agp_out = fopen(out, "w");
    write_break_agp(dict, breaks, bp_n, agp_out);
    fclose(agp_out);
    link_mat_destroy(link_mat);
    asm_destroy(dict);
    sd_destroy(sdict);
    int i;
    for (i = 0; i < bp_n; ++i)
        free(breaks[i].p);
    free(breaks);
    
    return bp_n;
}

static void print_asm_stats(uint64_t *n_stats, uint32_t *l_stats, int all)
{
#ifdef DEBUG
    int i;
    fprintf(stderr, "[I::%s] assembly stats:\n", __func__);
    for (i = 0; i < 10; ++i)
        fprintf(stderr, "[I::%s] N%d: %lu (n = %u)\n", __func__, (i + 1) * 10, n_stats[i], l_stats[i]);
#else
    fprintf(stderr, "[I::%s] assembly stats:\n", __func__);
    fprintf(stderr, "[I::%s]  N%d: %lu (n = %u)\n", __func__, 50, n_stats[4], l_stats[4]);
    fprintf(stderr, "[I::%s]  N%d: %lu (n = %u)\n", __func__, 90, n_stats[8], l_stats[8]);
    if (all)
        fprintf(stderr, "[I::%s]  N%d: %lu (n = %u)\n", __func__, 100, n_stats[9], l_stats[9]);
#endif
}

int run_yahs(char *fai, char *agp, char *link_file, uint32_t ml, uint8_t mq, char *out, int *resolutions, int nr, re_cuts_t *re_cuts, int no_contig_ec, int no_scaffold_ec, int no_mem_check)
{
    int ec_round, re, r, rc;
    char *out_fn, *out_agp, *out_agp_break;
    double noise;
    FILE *fo;
    sdict_t *sdict;
    asm_dict_t *dict;
    long rss_total, rss_limit;  
    
    ram_limit(&rss_total, &rss_limit);
    fprintf(stderr, "[I::%s] RAM total: %.3fGB\n", __func__, (double) rss_total / GB);
    fprintf(stderr, "[I::%s] RAM limit: %.3fGB\n", __func__, (double) rss_limit / GB);
    if (no_mem_check)
        fprintf(stderr, "[I::%s] RAM check disabled\n", __func__);

    sdict = make_sdict_from_index(fai, ml);
    out_fn = (char *) malloc(strlen(out) + 35);
    out_agp = (char *) malloc(strlen(out) + 35);
    out_agp_break = (char *) malloc(strlen(out) + 35);

    if (agp == 0 && no_contig_ec == 0) {
#ifdef DEBUG
        fprintf(stderr, "[DEBUG::%s] perform contig error break...\n", __func__);
#endif
        sprintf(out_agp_break, "%s_inital_break", out);
        ec_round = contig_error_break(fai, link_file, ml, out_agp_break);
        sprintf(out_agp_break, "%s_inital_break_%02d.agp", out, ec_round);
#ifdef DEBUG
        fprintf(stderr, "[DEBUG::%s] contig error break done\n", __func__);
#endif
    } else {
#ifdef DEBUG
        fprintf(stderr, "[DEBUG::%s] no contig error break...\n", __func__);
#endif
        if (agp != 0) {
#ifdef DEBUG
            fprintf(stderr, "[DEBUG::%s] use input AGP file\n", __func__);
#endif
            if (strlen(agp) > strlen(out)) {
                free(out_agp_break);
                out_agp_break = (char *) malloc(strlen(agp) + 35);
            }
            sprintf(out_agp_break, "%s", agp);
        } else {
#ifdef DEBUG
            fprintf(stderr, "[DEBUG::%s] make AGP file from input FASTA file\n", __func__);
#endif
            sprintf(out_agp_break, "%s_no_break.agp", out);
            write_sdict_to_agp(sdict, out_agp_break);
        }
    }

    r = rc = 0;
    
    dict = make_asm_dict_from_agp(sdict, out_agp_break);
    if (dict->n > MAX_N_SEQ) {
        fprintf(stderr, "[E::%s] sequence number exceeds limit (%d > %d)\n", __func__, dict->n, MAX_N_SEQ);
        fprintf(stderr, "[E::%s] consider removing short sequences before scaffolding, or\n", __func__);
        fprintf(stderr, "[E::%s] running without error correction (--no-contig-ec) if due to excessive contig error breaks\n", __func__);
        fprintf(stderr, "[E::%s] program halted...\n", __func__);
        return 1;
    }
    asm_sd_stats(dict, n_stats, l_stats);
    print_asm_stats(n_stats, l_stats, 1);
    asm_destroy(dict);

    while (r++ < nr) {
        fprintf(stderr, "[I::%s] scaffolding round %d resolution = %d\n", __func__, r, resolutions[r - 1]);
        
        dict = make_asm_dict_from_agp(sdict, out_agp_break);
        if (n_stats[4] < resolutions[r - 1] * 10) {
            if (rc) {
                fprintf(stderr, "[I::%s] assembly N50 (%lu) too small. End of scaffolding.\n", __func__, n_stats[4]);
                break;    
            } else {
                fprintf(stderr, "[W::%s] assembly N50 (%lu) too small. Scaffolding anyway...\n", __func__, n_stats[4]);
                fprintf(stderr, "[W::%s] consider running with increased memory limit if there was memory issue.\n", __func__);
            }
        }

        sprintf(out_fn, "%s_r%02d", out, r);
        // noise per unit
        re = run_scaffolding(fai, out_agp_break, link_file, ml, mq, re_cuts, out_fn, resolutions[r - 1], &noise, rss_limit, no_mem_check);
        if (!re) {
            sprintf(out_agp, "%s_r%02d.agp", out, r);
            if (no_scaffold_ec == 0) {
#ifdef DEBUG
                fprintf(stderr, "[DEBUG::%s] perform scaffold error break\n", __func__);
#endif

                sprintf(out_agp_break, "%s_r%02d_break.agp", out, r);
                scaffold_error_break(fai, link_file, ml, mq, out_agp, resolutions[r - 1], noise, out_agp_break);
#ifdef DEBUG
                fprintf(stderr, "[DEBUG::%s] scaffold error break done\n", __func__);
#endif

            } else {
#ifdef DEBUG
                fprintf(stderr, "[DEBUG::%s] no scaffold error break\n", __func__);
#endif
                sprintf(out_agp_break, "%s", out_agp);
            }
            ++rc;
        }
        asm_destroy(dict);

        fprintf(stderr, "[I::%s] scaffolding round %d done\n", __func__, r);

        dict = make_asm_dict_from_agp(sdict, out_agp_break);
        asm_sd_stats(dict, n_stats, l_stats);
        print_asm_stats(n_stats, l_stats, 0);
        asm_destroy(dict);
    }

#ifdef DEBUG
    fprintf(stderr, "[DEBUG::%s] make final output...\n", __func__);
#endif

    sprintf(out_agp, "%s_scaffolds_final.agp", out);
    // output sorted agp by scaffold size instead of file copy
    // file_copy(out_agp_break, out_agp);
    if (ml > 0) {
        // add short sequences to dict
#ifdef DEBUG
        fprintf(stderr, "[DEBUG::%s] add unused short sequences back...\n", __func__);
#endif
        sd_destroy(sdict);
        sdict = make_sdict_from_index(fai, 0);
        dict = make_asm_dict_from_agp(sdict, out_agp_break);
        add_unplaced_short_seqs(dict, ml);
    } else {
        dict = make_asm_dict_from_agp(sdict, out_agp_break);
    }
    fo = fopen(out_agp, "w");
    if (fo == NULL) {
        fprintf(stderr, "[E::%s] cannot open file %s for writing\n", __func__, out_agp);
        exit(EXIT_FAILURE);
    }
    write_sorted_agp(dict, fo);
    fclose(fo);
    
    asm_destroy(dict);
    sd_destroy(sdict);

    free(out_agp);
    free(out_fn);
    free(out_agp_break);

    return 0;
}

#ifndef DEBUG_GT4G
static int default_resolutions[15] = {10000, 20000, 50000, 100000, 200000, 500000, 1000000, 2000000, 5000000, 10000000, 20000000, 50000000, 100000000, 200000000, 500000000};
#else
static int default_resolutions[13] = {50000, 100000, 250000, 500000, 1000000, 2500000, 5000000, 10000000, 25000000, 50000000, 100000000, 250000000, 500000000};
#endif

static int default_nr(char *fai, uint32_t ml)
{
    int i, max_res, nr;
    int64_t genome_size;
    genome_size = 0;
    sdict_t *sdict = make_sdict_from_index(fai, ml);
    for (i = 0; i < sdict->n; ++i)
        genome_size += sdict->s[i].len;
    sd_destroy(sdict);
    
    max_res = 0;
    if (genome_size < 100000000)
        max_res = 1000000;
    else if (genome_size < 200000000)
        max_res = 2000000;
    else if (genome_size < 500000000)
        max_res = 5000000;
    else if (genome_size < 1000000000)
        max_res = 10000000;
    else if (genome_size < 2000000000)
        max_res = 20000000;
    else if (genome_size < 5000000000)
        max_res = 50000000;
    else if (genome_size < 10000000000)
        max_res = 100000000;
    else if (genome_size < 20000000000)
        max_res = 200000000;
    else
        max_res = 500000000;

    nr = 0;
    while (nr < sizeof(default_resolutions) / sizeof(int) && default_resolutions[nr] <= max_res)
        ++nr;

    return nr;
}

static void print_help(FILE *fp_help)
{
    fprintf(fp_help, "Usage: yahs [options] <contigs.fa> <hic.bed>|<hic.bam>|<hic.bin>\n");
    fprintf(fp_help, "Options:\n");
    fprintf(fp_help, "    -a FILE           AGP file (for rescaffolding) [none]\n");
    fprintf(fp_help, "    -r INT[,INT,...]  list of resolutions in ascending order [automate]\n");
    fprintf(fp_help, "    -e STR            restriction enzyme cutting sites [none]\n");
    fprintf(fp_help, "    -l INT            minimum length of a contig to scaffold [0]\n");
    fprintf(fp_help, "    -q INT            minimum mapping quality [10]\n");
    fprintf(fp_help, "    --no-contig-ec    do not do contig error correction\n");
    fprintf(fp_help, "    --no-scaffold-ec  do not do scaffold error correction\n");
    fprintf(fp_help, "    --no-mem-check    do not do memory check at runtime\n");
    fprintf(fp_help, "    -o STR            prefix of output files [yahs.out]\n");
    fprintf(fp_help, "    -v INT            verbose level [%d]\n", VERBOSE);
    fprintf(fp_help, "    --version         show version number\n");
}

static ko_longopt_t long_options[] = {
    { "no-contig-ec",   ko_no_argument, 301 },
    { "no-scaffold-ec", ko_no_argument, 302 },
    { "no-mem-check",   ko_no_argument, 303 },
    { "help",           ko_no_argument, 'h' },
    { "version",        ko_no_argument, 'V' },
    { 0, 0, 0 }
};

typedef struct {size_t n, m; char **a;} cstr_v;

int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_help(stderr);
        return 1;
    }

    liftrlimit();
    ys_realtime0 = realtime();

    char *fa, *fai, *agp, *link_file, *out, *restr, *ecstr, *ext, *link_bin_file, *agp_final, *fa_final;
    int *resolutions, nr, mq, ml, no_contig_ec, no_scaffold_ec, no_mem_check;

    const char *opt_str = "a:e:r:o:l:q:Vv:h";
    ketopt_t opt = KETOPT_INIT;

    int c, ret;
    FILE *fp_help = stderr;
    fa = fai = agp = link_file = out = restr = link_bin_file = agp_final = fa_final = 0;
    no_contig_ec = no_scaffold_ec = no_mem_check = 0;
    mq = 10;
    ml = 0;
    ecstr = 0;

    while ((c = ketopt(&opt, argc, argv, 1, opt_str, long_options)) >= 0) {
        if (c == 'a') {
            agp = opt.arg;
        } else if (c == 'r') {
            restr = opt.arg;
        } else if (c == 'o') {
            out = opt.arg;
        } else if (c == 'l') {
            ml = atoi(opt.arg);
        } else if (c == 'q') {
            mq = atoi(opt.arg);
        } else if (c == 'e') {
            // make a copy of ecstr to make sure the CMD correct
            ecstr = strdup(opt.arg);
        } else if (c == 301) {
            no_contig_ec = 1;
        } else if (c == 302) {
            no_scaffold_ec = 1;
        } else if (c == 303 ) {
            no_mem_check = 1;
        } else if (c == 'v') {
            VERBOSE = atoi(opt.arg);
        } else if (c == 'V') {
            puts(YAHS_VERSION);
            return 0;
        } else if (c == 'h') {
            fp_help = stdout;
        } else if (c == '?') {
            fprintf(stderr, "[E::%s] unknown option: \"%s\"\n", __func__, argv[opt.i - 1]);
            return 1;
        } else if (c == ':') {
            fprintf(stderr, "[E::%s] missing option: \"%s\"\n", __func__, argv[opt.i - 1]);
            return 1;
        }
    }

    if (fp_help == stdout) {
        print_help(stdout);
        return 0;
    }

    if (argc - opt.ind < 2) {
        fprintf(stderr, "[E::%s] missing input: two positional options required\n", __func__);
        print_help(stderr);
        return 1;
    }

    if (mq < 0 || mq > 255) {
        fprintf(stderr, "[E::%s] invalid mapping quality threshold: %d\n", __func__, mq);
        return 1;
    }

    if (ml < 0) {
        fprintf(stderr, "[E::%s] invalid contig length threshold: %d\n", __func__, ml);
        return 1;
    }

    uint8_t mq8;
    mq8 = (uint8_t) mq;

    fa = argv[opt.ind];
    link_file = argv[opt.ind + 1];

    fai = (char *) malloc(strlen(fa) + 5);
    sprintf(fai, "%s.fai", fa);

    if (agp)
        no_contig_ec = 1;
    
    if (restr) {
        // resolutions
        int max_n_res = 128;
        char  *eptr, *fptr;
        resolutions = (int *) malloc(max_n_res * sizeof(int));
        nr = 0;
        resolutions[nr++] = strtol(restr, &eptr, 10);
        while (*eptr != '\0') {
            if (nr == max_n_res) {
                fprintf(stderr, "[E::%s] more than %d resolutions specified. Is that really necessary?\n", __func__, max_n_res);
                exit(EXIT_FAILURE);
            }
            resolutions[nr++] = strtol(eptr + 1, &fptr, 10);
            eptr = fptr;
        }
    } else {
        resolutions = default_resolutions;
        nr = default_nr(fai, ml);
    }
    
    re_cuts_t *re_cuts;
    re_cuts = 0;
    if (ecstr) {
        // restriction enzymes cutting sites
        int i, n;
        char *pch;
        cstr_v enz_cs = {0, 0, 0};
        pch = strtok(ecstr, ",");
        while (pch != NULL) {
            n = -1;
            for (i = 0; i < strlen(pch); ++i) {
                c = pch[i];
                if (!isalpha(c)) {
                    fprintf(stderr, "[E::%s] non-alphabetic chacrater in restriction enzyme cutting site string: %s\n", __func__, pch);
                    exit(EXIT_FAILURE);
                }
                pch[i] = nucl_toupper[c];
                if (pch[i] == 'N') {
                    if (n >= 0) {
                        fprintf(stderr, "[E::%s] invalid restriction enzyme cutting site string (mutliple none-ACGT characters): %s\n", __func__, pch);
                        exit(EXIT_FAILURE);
                    }
                    n = i;
                }
            }
            if (n >= 0) {
                pch[n] = 'A';
                kv_push(char *, enz_cs, strdup(pch));
                pch[n] = 'C';
                kv_push(char *, enz_cs, strdup(pch));
                pch[n] = 'G';
                kv_push(char *, enz_cs, strdup(pch));
                pch[n] = 'T';
                kv_push(char *, enz_cs, strdup(pch));
            } else {
                kv_push(char *, enz_cs, strdup(pch));
            }
            pch = strtok(NULL, ",");
        }
#ifdef DEBUG
        fprintf(stderr, "[DEBUG::%s] list of restriction enzyme cutting sites (n = %ld)\n", __func__, enz_cs.n);
        for (i = 0; i < enz_cs.n; ++i)
            fprintf(stderr, "[DEBUG::%s] %s\n", __func__, enz_cs.a[i]);
#endif
        
        re_cuts = find_re_from_seqs(fa, ml, enz_cs.a, enz_cs.n);

        for (i = 0; i < enz_cs.n; ++i)
            free(enz_cs.a[i]);
        kv_destroy(enz_cs);

        free(ecstr);
    }

    if (out == 0)
        out = "yahs.out";

    ext = link_file + strlen(link_file) - 4;
    if (strcmp(ext, ".bam") == 0) {
        link_bin_file = malloc(strlen(out) + 5);
        sprintf(link_bin_file, "%s.bin", out);
        fprintf(stderr, "[I::%s] dump hic links (BAM) to binary file %s\n", __func__, link_bin_file);
        dump_links_from_bam_file(link_file, fai, ml, 0, link_bin_file);
    } else if (strcmp(ext, ".bed") == 0) {
        link_bin_file = malloc(strlen(out) + 5);
        sprintf(link_bin_file, "%s.bin", out);
        fprintf(stderr, "[I::%s] dump hic links (BED) to binary file %s\n", __func__, link_bin_file);
        dump_links_from_bed_file(link_file, fai, ml, 0, link_bin_file);
    } else if (strcmp(ext, ".bin") == 0) {
        link_bin_file = malloc(strlen(link_file) + 1);
        sprintf(link_bin_file, "%s", link_file);
        if (ml > 0)
            fprintf(stderr, "[W::%s] contig length threshold %d applied, make sure the binary file %s is up to date\n", __func__, ml, link_bin_file);
    } else {
        fprintf(stderr, "[E::%s] unknown link file format. File extension .bam, .bed or .bin is expected\n", __func__);
        exit(EXIT_FAILURE);
    }

#ifdef DEBUG_OPTIONS
    fprintf(stderr, "[DEBUG_OPTIONS::%s] list of options:\n", __func__);
    fprintf(stderr, "[DEBUG_OPTIONS::%s] fa:    %s\n", __func__, fa);
    fprintf(stderr, "[DEBUG_OPTIONS::%s] link:  %s\n", __func__, link_file);
    fprintf(stderr, "[DEBUG_OPTIONS::%s] linkb: %s\n", __func__, link_bin_file);
    fprintf(stderr, "[DEBUG_OPTIONS::%s] agp:   %s\n", __func__, agp);
    fprintf(stderr, "[DEBUG_OPTIONS::%s] res:   %s\n", __func__, restr);
    fprintf(stderr, "[DEBUG_OPTIONS::%s] RE:    %s\n", __func__, ecstr);
    fprintf(stderr, "[DEBUG_OPTIONS::%s] minl:  %d\n", __func__, ml);
    fprintf(stderr, "[DEBUG_OPTIONS::%s] minq:  %hhu\n", __func__, mq8);
    fprintf(stderr, "[DEBUG_OPTIONS::%s] nr:    %d\n", __func__, nr);
    int i;
    for (i = 0; i < nr; ++i)
        fprintf(stderr, "[DEBUG_OPTIONS::%s] nr=%d:  %d\n", __func__, i, resolutions[i]);
    fprintf(stderr, "[DEBUG_OPTIONS::%s] out:   %s\n", __func__, out);
    fprintf(stderr, "[DEBUG_OPTIONS::%s] ec[C]: %d\n", __func__, no_contig_ec);
    fprintf(stderr, "[DEBUG_OPTIONS::%s] ec[S]: %d\n", __func__, no_scaffold_ec);
#endif

    ret = run_yahs(fai, agp, link_bin_file, ml, mq8, out, resolutions, nr, re_cuts, no_contig_ec, no_scaffold_ec, no_mem_check);
    
    if (ret == 0) {
        agp_final = (char *) malloc(strlen(out) + 35);
        fa_final = (char *) malloc(strlen(out) + 35);
        sprintf(agp_final, "%s_scaffolds_final.agp", out);
        sprintf(fa_final, "%s_scaffolds_final.fa", out);
        fprintf(stderr, "[I::%s] writing FASTA file for scaffolds\n", __func__);
        FILE *fo;
        fo = fopen(fa_final, "w");
        if (fo == NULL) {
            fprintf(stderr, "[E::%s] cannot open file %s for writing\n", __func__, fa_final);
            exit(EXIT_FAILURE);
        }
        write_fasta_file_from_agp(fa, agp_final, fo, 60, 0);
        fclose(fo);

        sdict_t *sdict = make_sdict_from_index(fai, 0);
        asm_dict_t *dict = make_asm_dict_from_agp(sdict, agp_final);
        asm_sd_stats(dict, n_stats, l_stats);
        print_asm_stats(n_stats, l_stats, 1);
        asm_destroy(dict);
        sd_destroy(sdict);
    }

    if (fai)
        free(fai);

    if (restr)
        free(resolutions);

    if (link_bin_file)
        free(link_bin_file);
    
    if (fa_final)
        free(fa_final);

    if (agp_final)
        free(agp_final);

    if (re_cuts)
        re_cuts_destroy(re_cuts);

    fprintf(stderr, "[I::%s] Version: %s\n", __func__, YAHS_VERSION);
    fprintf(stderr, "[I::%s] CMD:", __func__);
    int i;
    for (i = 0; i < argc; ++i)
        fprintf(stderr, " %s", argv[i]);
    fprintf(stderr, "\n[I::%s] Real time: %.3f sec; CPU: %.3f sec; Peak RSS: %.3f GB\n", __func__, realtime() - ys_realtime0, cputime(), peakrss() / 1024.0 / 1024.0 / 1024.0);

    return ret;
}

