/* Native Linux CivNexus6.
 * Reads Civilization VI .fgx (Granny2) and exports .cn6.
 * import-cn6 overwrites a template FGX's vertices and triangles from CN6.
 * It cannot grow or shrink those arrays.
 * create-cn6 builds a new FGX and writes the CN6 skeleton into it.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "gr2.h"
#include "elements.h"
#include "typeinfo.h"
#include "virtual_ptr.h"
#include "compression.h"
#include "crc.h"

static TElementGeneric *child_at(TElementGeneric *e, int i)
{
    if (!e || i < 0 || (size_t)i >= e->children.count)
        return NULL;
    return *(TElementGeneric **)DArray_Get(&e->children, (size_t)i);
}

static TElementGeneric *child_named(TElementGeneric *e, const char *name)
{
    size_t i;
    if (!e)
        return NULL;
    for (i = 0; i < e->children.count; i++) {
        TElementGeneric *c = child_at(e, (int)i);
        if (c && c->name && strcmp(c->name, name) == 0)
            return c;
    }
    return NULL;
}

static int group_len(TElementGeneric *e)
{
    const char *first;
    int i;
    TElementGeneric *c0;
    if (!e || e->children.count < 1)
        return 0;
    c0 = child_at(e, 0);
    first = c0 ? c0->name : NULL;
    if (!first)
        return (int)e->children.count;
    for (i = 1; i < (int)e->children.count; i++) {
        TElementGeneric *c = child_at(e, i);
        if (c && c->name && strcmp(c->name, first) == 0)
            return i;
    }
    return (int)e->children.count;
}

static int group_count(TElementGeneric *e)
{
    int gl = group_len(e);
    if (gl < 1)
        return 0;
    return (int)e->children.count / gl;
}

static TElementGeneric *group_field(TElementGeneric *e, int rec, const char *field)
{
    int gl = group_len(e);
    int base, i;
    if (gl < 1)
        return NULL;
    base = rec * gl;
    for (i = 0; i < gl; i++) {
        TElementGeneric *c = child_at(e, base + i);
        if (c && c->name && strcmp(c->name, field) == 0)
            return c;
    }
    return NULL;
}

static const char *str_of(TElementGeneric *e)
{
    if (!e || e->rawInfo.type != TYPEID_STRING)
        return "";
    return ((TElementString *)e)->value ? ((TElementString *)e)->value : "";
}

static int i32_of(TElementGeneric *e, int dflt)
{
    if (!e || e->rawInfo.type != TYPEID_INT32 || !((TElementInt32 *)e)->value)
        return dflt;
    return ((TElementInt32 *)e)->value[0];
}

static int load_fgx(const char *path, TGr2 *gr2, uint8_t **keep)
{
    FILE *fp = fopen(path, "rb");
    long n;
    uint8_t *data;
    if (!fp)
        return 0;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    rewind(fp);
    data = malloc((size_t)n);
    if (!data || fread(data, (size_t)n, 1, fp) != 1) {
        fclose(fp);
        free(data);
        return 0;
    }
    fclose(fp);
    if (!Gr2_Init(gr2) || !Gr2_Load(data, (size_t)n, gr2)) {
        free(data);
        return 0;
    }
    *keep = data;
    return 1;
}

typedef struct {
    char name[80];
    int type;
    int count;
    int offset;
    int length;
} Field;

static int decode_fields(TGr2 *gr2, TElementArray *arr, Field *fields, int max, int *stride)
{
    void *type;
    uint64_t off = 0;
    int cursor = 0, n = 0;
    int is64;
    if (!arr)
        return 0;
    is64 = gr2->bitsSize == 64;
    type = decode_ptr(&gr2->virtual_ptr, (uint32_t)arr->offset);
    if (!type)
        return 0;
    while (n < max) {
        TNodeTypeInfo info;
        const char *nm;
        int count, len, align;
        if (!TypeInfo_Parse((const uint8_t *)type, &info, is64, &off))
            break;
        if (info.type == 0 || info.type >= TYPEID_MAX)
            break;
        nm = (const char *)decode_ptr(&gr2->virtual_ptr, (uint32_t)info.nameOffset);
        count = info.arraySize > 0 ? info.arraySize : 1;
        len = (int)(is64 ? ELEMENT_TYPE_INFO[info.type].size64 : ELEMENT_TYPE_INFO[info.type].size32);
        if (len < 1)
            len = 1;
        align = len >= 4 ? 4 : len;
        if (align > 1 && (cursor % align))
            cursor += align - (cursor % align);
        snprintf(fields[n].name, sizeof fields[n].name, "%s", nm ? nm : "");
        fields[n].type = (int)info.type;
        fields[n].count = count;
        fields[n].offset = cursor;
        fields[n].length = len;
        cursor += len * count;
        n++;
    }
    if (cursor % 4)
        cursor += 4 - (cursor % 4);
    *stride = cursor;
    return n;
}

static const Field *field_named(Field *f, int n, const char *name)
{
    int i;
    for (i = 0; i < n; i++)
        if (strcmp(f[i].name, name) == 0)
            return &f[i];
    return NULL;
}

static float read_f(const uint8_t *p)
{
    float v;
    memcpy(&v, p, 4);
    return v;
}

static void read_vec(const uint8_t *base, const Field *f, int which, float *out, int n, float fill)
{
    int i;
    for (i = 0; i < n; i++)
        out[i] = fill;
    if (!f || which < 0)
        return;
    for (i = 0; i < n && i < f->count; i++) {
        const uint8_t *p = base + f->offset + i * f->length;
        if (f->type == TYPEID_REAL32)
            out[i] = read_f(p);
        else if (f->type == TYPEID_UINT8 || f->type == TYPEID_NORMALUINT8)
            out[i] = (float)p[0];
        else if (f->type == TYPEID_UINT16 || f->type == TYPEID_NORMALUINT16)
            out[i] = (float)(p[0] | (p[1] << 8));
    }
}

static int bone_id_by_name(TElementGeneric *bones, const char *name)
{
    int i, n;
    if (!name || !name[0] || !bones)
        return 0;
    n = group_count(bones);
    for (i = 0; i < n; i++) {
        const char *bn = str_of(group_field(bones, i, "Name"));
        if (bn && strcmp(bn, name) == 0)
            return i;
    }
    return 0;
}

static int export_cn6(TGr2 *gr2, const char *out_path, char *log, size_t log_len)
{
    FILE *fp;
    TElementGeneric *skels, *skel, *bones, *meshes;
    int bi, mi, mesh_count, bone_count;
    skels = child_named(gr2->root, "FxsSkeletons");
    meshes = child_named(gr2->root, "FxsMeshes");
    if (!skels || !meshes || group_count(skels) < 1) {
        snprintf(log, log_len, "This FGX has no skeleton or mesh.");
        return 0;
    }
    bones = group_field(skels, 0, "Bones");
    if (!bones) {
        snprintf(log, log_len, "Skeleton has no bones.");
        return 0;
    }
    fp = fopen(out_path, "w");
    if (!fp) {
        snprintf(log, log_len, "Cannot write %s", out_path);
        return 0;
    }
    bone_count = group_count(bones);
    mesh_count = group_count(meshes);
    fprintf(fp, "// CivNexus6 CN6 - Exported from CivNexus6 1.3.3 Linux\n");
    fprintf(fp, "skeleton\n");
    for (bi = 0; bi < bone_count; bi++) {
        TElementGeneric *b = NULL;
        TElementGeneric *tr = group_field(bones, bi, "Transform");
        TElementGeneric *inv = group_field(bones, bi, "InverseWorldTransform");
        const char *name = str_of(group_field(bones, bi, "Name"));
        int parent = i32_of(group_field(bones, bi, "ParentIndex"), -1);
        float pos[3] = {0, 0, 0};
        float rot[4] = {0, 0, 0, 1};
        int k;
        if (tr && tr->rawInfo.type == TYPEID_TRANSFORM && ((TElementTransform *)tr)->value) {
            TTransformation *t = ((TElementTransform *)tr)->value;
            pos[0] = t->translation[0];
            pos[1] = t->translation[1];
            pos[2] = t->translation[2];
            rot[0] = t->rotation[0];
            rot[1] = t->rotation[1];
            rot[2] = t->rotation[2];
            rot[3] = t->rotation[3];
        }
        fprintf(fp, "%d \"%s\" %d %.9g %.9g %.9g %.9g %.9g %.9g %.9g",
                bi, name, parent, pos[0], pos[1], pos[2], rot[0], rot[1], rot[2], rot[3]);
        if (inv && inv->rawInfo.type == TYPEID_REAL32 && ((TElementFloat *)inv)->value) {
            int n = inv->rawInfo.arraySize > 0 ? inv->rawInfo.arraySize : 16;
            if (n > 16)
                n = 16;
            for (k = 0; k < n; k++)
                fprintf(fp, " %.9g", ((TElementFloat *)inv)->value[k]);
            for (; k < 16; k++)
                fprintf(fp, " 0");
        } else {
            for (k = 0; k < 16; k++)
                fprintf(fp, " %s", (k % 5 == 0) ? "1" : "0");
        }
        fprintf(fp, "\n");
    }
    fprintf(fp, "meshes:%d\n", mesh_count);
    for (mi = 0; mi < mesh_count; mi++) {
        TElementGeneric *mesh = child_at(meshes, mi);
        TElementGeneric *vdata_ref, *topo_ref, *binds;
        TElementGeneric *verts_el, *names_el;
        TElementArray *verts;
        Field fields[24];
        int nfields, stride = 0, vi, vcount, ti;
        const Field *fpos, *fnrm, *ftan, *fbin, *fuv, *fw, *fi;
        const char *mesh_name = str_of(group_field(meshes, mi, "Name"));
        const uint8_t *vbytes;
        TElementGeneric *indices = NULL;
        if (!mesh_name[0])
            mesh_name = "mesh";
        fprintf(fp, "mesh:\"%s\"\n", mesh_name);
        fprintf(fp, "materials\n");
        {
            TElementGeneric *mb = group_field(meshes, mi, "MaterialBindings");
            int mbi, wrote = 0;
            if (mb) {
                for (mbi = 0; mbi < (int)mb->children.count; mbi++) {
                    const char *mn = str_of(child_named(child_at(mb, mbi), "Name"));
                    if (!mn[0])
                        mn = "Default_Material";
                    fprintf(fp, "\"%s\"\n", mn);
                    wrote++;
                }
            }
            if (!wrote)
                fprintf(fp, "\"Default_Material\"\n");
        }
        vdata_ref = group_field(meshes, mi, "PrimaryVertexData");
        if (!vdata_ref)
            vdata_ref = group_field(meshes, mi, "VertexData");
        verts_el = NULL;
        if (vdata_ref && vdata_ref->children.count)
            verts_el = child_named(vdata_ref, "Vertices");
        if (!verts_el) {
            TElementGeneric *vdatas = child_named(gr2->root, "FxsVertexDatas");
            if (vdatas)
                verts_el = group_field(vdatas, mi, "Vertices");
        }
        names_el = NULL;
        if (vdata_ref)
            names_el = child_named(vdata_ref, "VertexComponentNames");
        verts = (TElementArray *)verts_el;
        nfields = verts_el ? decode_fields(gr2, verts, fields, 24, &stride) : 0;
        fpos = field_named(fields, nfields, "Position");
        fnrm = field_named(fields, nfields, "Normal");
        ftan = field_named(fields, nfields, "Tangent");
        fbin = field_named(fields, nfields, "Binormal");
        fuv = field_named(fields, nfields, "TextureCoordinates0");
        if (!fuv)
            fuv = field_named(fields, nfields, "map1");
        fw = field_named(fields, nfields, "BoneWeights");
        fi = field_named(fields, nfields, "BoneIndices");
        vcount = verts_el ? (int)verts_el->size : 0;
        vbytes = verts ? (const uint8_t *)verts->data : NULL;
        fprintf(fp, "vertices\n");
        binds = group_field(meshes, mi, "BoneBindings");
        for (vi = 0; vi < vcount; vi++) {
            const uint8_t *base = vbytes + (size_t)vi * (size_t)stride;
            float p[3], nrm[3], tan[3], bin[3], uv[2];
            int ids[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            int wts[8] = {255, 0, 0, 0, 0, 0, 0, 0};
            int z;
            read_vec(base, fpos, 0, p, 3, 0);
            read_vec(base, fnrm, 0, nrm, 3, 0);
            read_vec(base, ftan, 0, tan, 3, 0);
            if (!ftan) {
                tan[0] = nrm[0];
                tan[1] = nrm[1];
                tan[2] = nrm[2];
            }
            read_vec(base, fbin, 0, bin, 3, 0);
            if (!fbin) {
                bin[0] = nrm[0];
                bin[1] = nrm[1];
                bin[2] = nrm[2];
            }
            read_vec(base, fuv, 0, uv, 2, 0);
            if (fw) {
                for (z = 0; z < 8 && z < fw->count; z++) {
                    const uint8_t *bp = base + fw->offset + z * fw->length;
                    if (fw->type == TYPEID_UINT8 || fw->type == TYPEID_NORMALUINT8)
                        wts[z] = bp[0];
                    else if (fw->type == TYPEID_REAL32)
                        wts[z] = (int)(read_f(bp) * 255.0f + 0.5f);
                }
            }
            if (fi) {
                for (z = 0; z < 8 && z < fi->count; z++) {
                    const uint8_t *bp = base + fi->offset + z * fi->length;
                    int local = 0;
                    const char *bname = NULL;
                    if (fi->type == TYPEID_UINT8 || fi->type == TYPEID_NORMALUINT8)
                        local = bp[0];
                    else if (fi->type == TYPEID_UINT16 || fi->type == TYPEID_NORMALUINT16)
                        local = bp[0] | (bp[1] << 8);
                    else if (fi->type == TYPEID_INT32)
                        memcpy(&local, bp, 4);
                    if (binds && local >= 0 && local < group_count(binds)) {
                        bname = str_of(group_field(binds, local, "BoneName"));
                        if (!bname[0])
                            bname = str_of(group_field(binds, local, "Name"));
                        if (!bname[0]) {
                            TElementGeneric *bb = child_at(binds, local * group_len(binds));
                            if (bb && bb->rawInfo.type == TYPEID_STRING)
                                bname = str_of(bb);
                        }
                    }
                    ids[z] = bname && bname[0] ? bone_id_by_name(bones, bname) : local;
                }
            }
            fprintf(fp,
                    "%.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g 0 0 0 0 "
                    "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
                    p[0], p[1], p[2], nrm[0], nrm[1], nrm[2], tan[0], tan[1], tan[2], bin[0], bin[1], bin[2],
                    uv[0], uv[1], ids[0], ids[1], ids[2], ids[3], ids[4], ids[5], ids[6], ids[7],
                    wts[0], wts[1], wts[2], wts[3], wts[4], wts[5], wts[6], wts[7]);
        }
        topo_ref = group_field(meshes, mi, "PrimaryTopology");
        if (topo_ref)
            indices = child_named(topo_ref, "Indices16");
        if (!indices && topo_ref)
            indices = child_named(topo_ref, "Indices");
        if (!indices) {
            TElementGeneric *topos = child_named(gr2->root, "FxsTriTopologies");
            if (topos && group_count(topos)) {
                indices = group_field(topos, mi, "Indices16");
                if (!indices)
                    indices = group_field(topos, mi, "Indices");
            }
        }
        fprintf(fp, "triangles\n");
        if (indices) {
            int nidx = (int)indices->size;
            int use16 = indices->name && strcmp(indices->name, "Indices16") == 0;
            const uint8_t *ib = (const uint8_t *)((TElementArray *)indices)->data;
            int group = 0;
            if (!ib && indices->children.count) {
                TElementGeneric *leaf = child_at(indices, 0);
                if (leaf && leaf->rawInfo.type == TYPEID_INT16 && ((TElementInt16 *)leaf)->value) {
                    ib = (const uint8_t *)((TElementInt16 *)leaf)->value;
                    if (nidx < 3)
                        nidx = leaf->rawInfo.arraySize > 0 ? leaf->rawInfo.arraySize : (int)leaf->size;
                    use16 = 1;
                }
            }
            for (ti = 0; ib && ti + 2 < nidx; ti += 3) {
                int a, b, c;
                if (use16) {
                    a = ib[ti * 2] | (ib[ti * 2 + 1] << 8);
                    b = ib[(ti + 1) * 2] | (ib[(ti + 1) * 2 + 1] << 8);
                    c = ib[(ti + 2) * 2] | (ib[(ti + 2) * 2 + 1] << 8);
                } else {
                    memcpy(&a, ib + ti * 4, 4);
                    memcpy(&b, ib + (ti + 1) * 4, 4);
                    memcpy(&c, ib + (ti + 2) * 4, 4);
                }
                fprintf(fp, "%d %d %d %d\n", a, b, c, group);
            }
        }
    }
    fprintf(fp, "end\n");
    fclose(fp);
    snprintf(log, log_len, "Wrote %s  (%d meshes, %d bones)", out_path, mesh_count, bone_count);
    return 1;
}

static void info_text(TGr2 *gr2, const char *path, char *out, size_t n)
{
    TElementGeneric *art = child_named(gr2->root, "FxsArtToolInfo");
    TElementGeneric *exp = child_named(gr2->root, "FxsExportInfo");
    TElementGeneric *from = child_named(gr2->root, "FxsFromFileName");
    TElementGeneric *meshes = child_named(gr2->root, "FxsMeshes");
    TElementGeneric *skels = child_named(gr2->root, "FxsSkeletons");
    TElementGeneric *anims = child_named(gr2->root, "FxsAnimations");
    TElementGeneric *mats = child_named(gr2->root, "FxsMaterials");
    int len = 0;
    len += snprintf(out + len, n - (size_t)len, "File\n  %s\n\n", path);
    len += snprintf(out + len, n - (size_t)len, "Art tool\n  %s %d.%d   units/m %s\n",
                    str_of(child_named(art, "FromArtToolName")),
                    i32_of(child_named(art, "ArtToolMajorRevision"), 0),
                    i32_of(child_named(art, "ArtToolMinorRevision"), 0),
                    "see FGX");
    if (art) {
        TElementGeneric *u = child_named(art, "UnitsPerMeter");
        if (u && u->rawInfo.type == TYPEID_REAL32 && ((TElementFloat *)u)->value)
            len += snprintf(out + len, n - (size_t)len, "  UnitsPerMeter %.4g\n", ((TElementFloat *)u)->value[0]);
    }
    len += snprintf(out + len, n - (size_t)len, "\nExporter\n  %s %d.%d custom %d build %d\n",
                    str_of(child_named(exp, "ExporterName")),
                    i32_of(child_named(exp, "ExporterMajorRevision"), 0),
                    i32_of(child_named(exp, "ExporterMinorRevision"), 0),
                    i32_of(child_named(exp, "ExporterCustomization"), 0),
                    i32_of(child_named(exp, "ExporterBuildNumber"), 0));
    len += snprintf(out + len, n - (size_t)len, "\nFrom\n  %s\n", str_of(from));
    len += snprintf(out + len, n - (size_t)len, "\nCounts\n  meshes %d\n  skeletons %d\n  materials %d\n  animations %d\n",
                    meshes ? group_count(meshes) : 0,
                    skels ? group_count(skels) : 0,
                    mats ? group_count(mats) : 0,
                    anims ? group_count(anims) : 0);
    if (skels && group_count(skels)) {
        TElementGeneric *bones = group_field(skels, 0, "Bones");
        int i;
        len += snprintf(out + len, n - (size_t)len, "\nSkeleton %s\n", str_of(group_field(skels, 0, "Name")));
        if (bones) {
            int show = group_count(bones);
            if (show > 24)
                show = 24;
            for (i = 0; i < show; i++)
                len += snprintf(out + len, n - (size_t)len, "  %d  %s   parent %d\n",
                                i, str_of(group_field(bones, i, "Name")), i32_of(group_field(bones, i, "ParentIndex"), -1));
            if (group_count(bones) > show)
                len += snprintf(out + len, n - (size_t)len, "  ... %d bones\n", group_count(bones));
        }
    }
    if (meshes && group_count(meshes)) {
        int i, show = group_count(meshes);
        if (show > 16)
            show = 16;
        len += snprintf(out + len, n - (size_t)len, "\nMeshes\n");
        for (i = 0; i < show; i++)
            len += snprintf(out + len, n - (size_t)len, "  %s\n", str_of(group_field(meshes, i, "Name")));
    }
    (void)len;
}

static int export_na2(TGr2 *gr2, const char *out_path, char *log, size_t log_len)
{
    FILE *fp;
    TElementGeneric *anims = child_named(gr2->root, "FxsAnimations");
    TElementGeneric *skels = child_named(gr2->root, "FxsSkeletons");
    TElementGeneric *bones;
    int i, n;
    if (!anims || anims->children.count < 1) {
        snprintf(log, log_len, "No animations in this FGX. Open an .fgx/.anm animation, not a model template.");
        return 0;
    }
    bones = skels && skels->children.count ? child_named(child_at(skels, 0), "Bones") : NULL;
    fp = fopen(out_path, "w");
    if (!fp) {
        snprintf(log, log_len, "Cannot write %s", out_path);
        return 0;
    }
    n = bones ? (int)bones->children.count : 0;
    fprintf(fp, "0.0\n");
    fprintf(fp, "%d\n", n);
    for (i = 0; i < n; i++)
        fprintf(fp, "%s\n", str_of(child_named(child_at(bones, i), "Name")));
    fprintf(fp, "%zu\n", anims->children.count);
    for (i = 0; i < (int)anims->children.count; i++) {
        TElementGeneric *a = child_at(anims, i);
        fprintf(fp, "# %s\n", str_of(child_named(a, "Name")));
    }
    fclose(fp);
    snprintf(log, log_len, "Wrote animation index %s (%zu clips). Curve sampling is not in this build.", out_path, anims->children.count);
    return 1;
}

static int import_cn6(TGr2 *gr2, const uint8_t *file, size_t flen, const char *cn6_path, const char *out_path, char *log, size_t log_len);
static int create_cn6(const char *cn6_path, const char *out_path, char *log, size_t log_len);

#ifdef CIVNEXUS_GUI
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

static Display *dpy;
static Window win;
static GC gc, gc_btn, gc_text, gc_dim, gc_hi;
static XFontStruct *font;
static char current_path[1024];
static char entry[1024];
static char status[512];
static char body[16000];
static char files[48][768];
static int nfiles, selected = -1, body_scroll;
static TGr2 loaded;
static uint8_t *loaded_bytes;
static size_t loaded_len;
static int have_file;

static void set_status(const char *s)
{
    snprintf(status, sizeof status, "%s", s);
}

static void share_dir(char *out, size_t n, const char *sub)
{
    char exe[1024];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    char *slash;
    if (k < 0) {
        snprintf(out, n, "%s", sub);
        return;
    }
    exe[k] = 0;
    slash = strrchr(exe, '/');
    if (slash)
        *slash = 0;
    snprintf(out, n, "%s/../share/%s", exe, sub);
}

static void refresh_body(void)
{
    if (!have_file) {
        snprintf(body, sizeof body,
                 "CivNexus6 for Linux\n\n"
                 "1. Click a template on the left, or type a path below, then Open.\n"
                 "2. Export CN6. Edit that file in Blender.\n"
                 "3. Overwrite keeps the template size. Create builds a new FGX at the CN6 size.\n\n"
                 "Pick a .cn6 in the path bar, then Create.\n"
                 "Cooking .geo and .tex is not in this program.\n");
        return;
    }
    info_text(&loaded, current_path, body, sizeof body);
}

static void close_file(void)
{
    if (have_file) {
        Gr2_Free(&loaded);
        free(loaded_bytes);
        loaded_bytes = NULL;
        loaded_len = 0;
        have_file = 0;
    }
}

static int open_path(const char *path)
{
    TGr2 g;
    uint8_t *bytes = NULL;
    struct stat st;
    if (!path || !path[0]) {
        set_status("Type a path or pick a file.");
        return 0;
    }
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        set_status("That path is not a file.");
        return 0;
    }
    if (!load_fgx(path, &g, &bytes)) {
        set_status("Could not read that FGX.");
        return 0;
    }
    close_file();
    loaded = g;
    loaded_bytes = bytes;
    loaded_len = (size_t)st.st_size;
    have_file = 1;
    snprintf(current_path, sizeof current_path, "%s", path);
    snprintf(entry, sizeof entry, "%s", path);
    set_status(path);
    body_scroll = 0;
    refresh_body();
    return 1;
}

static void add_dir(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    if (!d)
        return;
    while ((de = readdir(d)) && nfiles < 48) {
        size_t n = strlen(de->d_name);
        const char *ext = n > 4 ? de->d_name + n - 4 : "";
        if (!strcmp(ext, ".fgx") || !strcmp(ext, ".FGX") || !strcmp(ext, ".cn6") || !strcmp(ext, ".na2") || !strcmp(ext, ".gr2"))
            snprintf(files[nfiles++], sizeof files[0], "%s/%s", dir, de->d_name);
    }
    closedir(d);
}

static void rescan(void)
{
    char pantry[1024], cwd[1024];
    nfiles = 0;
    share_dir(pantry, sizeof pantry, "pantry");
    add_dir(pantry);
    share_dir(pantry, sizeof pantry, "civnexus6");
    add_dir(pantry);
    if (getcwd(cwd, sizeof cwd))
        add_dir(cwd);
}

static void swap_ext(char *path, size_t n, const char *ext)
{
    char *dot = strrchr(path, '.');
    char *slash = strrchr(path, '/');
    if (dot && (!slash || dot > slash))
        snprintf(dot, n - (size_t)(dot - path), "%s", ext);
    else
        strncat(path, ext, n - strlen(path) - 1);
}

static void on_export_cn6(void)
{
    char out[1100], log[256];
    if (!have_file) {
        set_status("Open an FGX first.");
        return;
    }
    snprintf(out, sizeof out, "%s", current_path);
    swap_ext(out, sizeof out, ".cn6");
    export_cn6(&loaded, out, log, sizeof log);
    set_status(log);
    snprintf(entry, sizeof entry, "%s", out);
    rescan();
}

static void on_export_na2(void)
{
    char out[1100], log[256];
    if (!have_file) {
        set_status("Open an FGX first.");
        return;
    }
    snprintf(out, sizeof out, "%s", current_path);
    swap_ext(out, sizeof out, ".na2");
    export_na2(&loaded, out, log, sizeof log);
    set_status(log);
}

static void on_overwrite(void)
{
    char cn6[1024], out[1100], log[512];
    if (!have_file) {
        set_status("Open an FGX template first.");
        return;
    }
    if (strlen(entry) > 4 && strcmp(entry + strlen(entry) - 4, ".cn6") == 0)
        snprintf(cn6, sizeof cn6, "%s", entry);
    else {
        snprintf(cn6, sizeof cn6, "%s", current_path);
        swap_ext(cn6, sizeof cn6, ".cn6");
    }
    snprintf(out, sizeof out, "%s", current_path);
    swap_ext(out, sizeof out, ".out.fgx");
    if (import_cn6(&loaded, loaded_bytes, loaded_len, cn6, out, log, sizeof log))
        snprintf(entry, sizeof entry, "%s", out);
    set_status(log);
    rescan();
}

static void on_create(void)
{
    char cn6[1024], out[1100], log[512];
    if (!(strlen(entry) > 4 && strcmp(entry + strlen(entry) - 4, ".cn6") == 0)) {
        set_status("Pick a .cn6, then Create.");
        return;
    }
    snprintf(cn6, sizeof cn6, "%s", entry);
    snprintf(out, sizeof out, "%s", cn6);
    swap_ext(out, sizeof out, ".fgx");
    if (!create_cn6(cn6, out, log, sizeof log)) {
        set_status(log);
        return;
    }
    set_status(log);
    rescan();
    open_path(out);
}

static void on_blender(void)
{
    char exe[1024], blender[1024];
    ssize_t k;
    pid_t pid;
    k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k < 0) {
        set_status("Cannot find the bundled Blender.");
        return;
    }
    exe[k] = 0;
    {
        char *slash = strrchr(exe, '/');
        if (slash)
            *slash = 0;
    }
    snprintf(blender, sizeof blender, "%s/../blender/blender", exe);
    if (access(blender, 1) != 0) {
        set_status("Blender is not in this package.");
        return;
    }
    pid = fork();
    if (pid == 0) {
        if (have_file && strlen(entry) > 4 && strcmp(entry + strlen(entry) - 4, ".cn6") == 0) {
            execl(blender, "blender", entry, (char *)NULL);
        } else {
            execl(blender, "blender", (char *)NULL);
        }
        _exit(127);
    }
    if (pid < 0)
        set_status("Could not start Blender.");
    else
        set_status("Blender is open. Enable the CN6 addon if it is not already.");
}

static int hit_button(int x, int y)
{
    if (y < 8 || y > 44)
        return 0;
    if (x >= 12 && x < 92)
        return 1;
    if (x >= 100 && x < 230)
        return 2;
    if (x >= 238 && x < 368)
        return 3;
    if (x >= 376 && x < 516)
        return 4;
    if (x >= 524 && x < 624)
        return 5;
    if (x >= 632 && x < 732)
        return 6;
    return 0;
}

static void draw(void)
{
    XWindowAttributes wa;
    int y, i, line_h, row;
    const char *p;
    char line[180];
    const char *labels[] = {"Open", "Export CN6", "Export NA2", "Overwrite", "Create", "Blender"};
    int bx[] = {12, 100, 238, 376, 524, 632};
    int bw[] = {80, 130, 130, 140, 100, 100};
    XGetWindowAttributes(dpy, win, &wa);
    line_h = font ? font->ascent + font->descent + 3 : 16;
    XSetForeground(dpy, gc, 0x1c1a17);
    XFillRectangle(dpy, win, gc, 0, 0, wa.width, wa.height);
    XSetForeground(dpy, gc, 0x2a261f);
    XFillRectangle(dpy, win, gc, 0, 0, wa.width, 52);
    XFillRectangle(dpy, win, gc, 0, 52, 320, wa.height - 52);
    for (i = 0; i < 6; i++) {
        XSetForeground(dpy, gc_btn, 0xe8dcc8);
        XFillRectangle(dpy, win, gc_btn, bx[i], 10, bw[i], 30);
        XSetForeground(dpy, gc_text, 0x1c1a17);
        XDrawString(dpy, win, gc_text, bx[i] + 14, 30, labels[i], (int)strlen(labels[i]));
    }
    XSetForeground(dpy, gc_dim, 0xd9cbb6);
    XDrawString(dpy, win, gc_dim, 760, 30, "CivNexus6", 9);
    y = 74;
    for (i = 0; i < nfiles && y < wa.height - 56; i++) {
        const char *base = strrchr(files[i], '/');
        base = base ? base + 1 : files[i];
        if (i == selected) {
            XSetForeground(dpy, gc_hi, 0x4a4034);
            XFillRectangle(dpy, win, gc_hi, 8, y - 12, 304, line_h);
        }
        XSetForeground(dpy, gc_text, 0xf3eadc);
        snprintf(line, sizeof line, "%s", base);
        if (strlen(line) > 34)
            line[34] = 0;
        XDrawString(dpy, win, gc_text, 16, y, line, (int)strlen(line));
        y += line_h;
    }
    y = 74;
    p = body;
    row = 0;
    while (*p && y < wa.height - 56) {
        i = 0;
        while (p[i] && p[i] != '\n' && i < 90)
            i++;
        if (row >= body_scroll) {
            memcpy(line, p, (size_t)i);
            line[i] = 0;
            XSetForeground(dpy, gc_text, 0xf3eadc);
            XDrawString(dpy, win, gc_text, 336, y, line, i);
            y += line_h;
        }
        row++;
        p += i;
        if (*p == '\n')
            p++;
    }
    XSetForeground(dpy, gc, 0x3a342c);
    XFillRectangle(dpy, win, gc, 0, wa.height - 48, wa.width, 48);
    XSetForeground(dpy, gc_btn, 0x141210);
    XFillRectangle(dpy, win, gc_btn, 12, wa.height - 38, wa.width - 24, 24);
    XSetForeground(dpy, gc_text, 0xf3eadc);
    snprintf(line, sizeof line, "%s", entry[0] ? entry : "path");
    if (strlen(line) > 100) {
        memmove(line, line + strlen(line) - 100, 101);
    }
    XDrawString(dpy, win, gc_text, 18, wa.height - 21, line, (int)strlen(line));
    XSetForeground(dpy, gc_dim, 0xc8bba4);
    XDrawString(dpy, win, gc_dim, 12, 64, status, (int)strlen(status) > 42 ? 42 : (int)strlen(status));
}

static int run_gui(const char *initial)
{
    XEvent ev;
    Atom wm_delete;
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "No DISPLAY. Use: civnexus6 export-cn6 in.fgx out.cn6\n");
        return 1;
    }
    font = XLoadQueryFont(dpy, "fixed");
    if (!font)
        font = XLoadQueryFont(dpy, "6x13");
    win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 40, 40, 1040, 700, 0, 0, 0x1c1a17);
    XStoreName(dpy, win, "CivNexus6");
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    gc = XCreateGC(dpy, win, 0, NULL);
    gc_btn = XCreateGC(dpy, win, 0, NULL);
    gc_text = XCreateGC(dpy, win, 0, NULL);
    gc_dim = XCreateGC(dpy, win, 0, NULL);
    gc_hi = XCreateGC(dpy, win, 0, NULL);
    if (font) {
        XSetFont(dpy, gc_text, font->fid);
        XSetFont(dpy, gc_dim, font->fid);
    }
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | KeyPressMask | StructureNotifyMask);
    XMapWindow(dpy, win);
    rescan();
    set_status("Pick a template");
    refresh_body();
    if (initial && initial[0])
        open_path(initial);
    for (;;) {
        int button;
        XNextEvent(dpy, &ev);
        if (ev.type == Expose || ev.type == ConfigureNotify)
            draw();
        else if (ev.type == ClientMessage)
            break;
        else if (ev.type == ButtonPress) {
            int x = ev.xbutton.x, y = ev.xbutton.y;
            int line_h = font ? font->ascent + font->descent + 3 : 16;
            if (ev.xbutton.button == 4 && body_scroll > 0)
                body_scroll--;
            if (ev.xbutton.button == 5)
                body_scroll++;
            button = hit_button(x, y);
            if (button == 1)
                open_path(entry);
            else if (button == 2)
                on_export_cn6();
            else if (button == 3)
                on_export_na2();
            else if (button == 4)
                on_overwrite();
            else if (button == 5)
                on_create();
            else if (button == 6)
                on_blender();
            else if (x < 320 && y > 68) {
                int idx = (y - 74) / line_h;
                if (idx >= 0 && idx < nfiles) {
                    selected = idx;
                    snprintf(entry, sizeof entry, "%s", files[idx]);
                    if (strlen(entry) > 4 && (strcmp(entry + strlen(entry) - 4, ".fgx") == 0 || strcmp(entry + strlen(entry) - 4, ".FGX") == 0))
                        open_path(entry);
                }
            }
            draw();
        } else if (ev.type == KeyPress) {
            char buf[32];
            KeySym ks;
            int n = XLookupString(&ev.xkey, buf, sizeof buf - 1, &ks, NULL);
            if (ks == XK_Escape)
                break;
            if (ks == XK_Up && body_scroll > 0)
                body_scroll--;
            else if (ks == XK_Down)
                body_scroll++;
            else if (ks == XK_Return)
                open_path(entry);
            else if (ks == XK_o || ks == XK_O)
                on_export_cn6();
            else if (ks == XK_w || ks == XK_W)
                on_overwrite();
            else if (ks == XK_b || ks == XK_B)
                on_blender();
            else if (ks == XK_BackSpace) {
                size_t nlen = strlen(entry);
                if (nlen)
                    entry[nlen - 1] = 0;
            } else if (n == 1 && buf[0] >= 32 && buf[0] < 127 && strlen(entry) + 1 < sizeof entry) {
                size_t nlen = strlen(entry);
                entry[nlen] = buf[0];
                entry[nlen + 1] = 0;
            }
            draw();
        }
    }
    close_file();
    XCloseDisplay(dpy);
    return 0;
}
#endif

static int decompress_raw(const uint8_t *file, size_t flen, TGr2 *gr2, uint8_t **out, char *log, size_t log_len)
{
    uint8_t *raw;
    uint32_t i;
    if (!gr2->dataSize) {
        snprintf(log, log_len, "FGX has no sector data.");
        return 0;
    }
    raw = calloc(1, gr2->dataSize);
    if (!raw) {
        snprintf(log, log_len, "Out of memory.");
        return 0;
    }
    for (i = 0; i < gr2->fileInfo.sectorCount; i++) {
        TSector sector = gr2->sectors[i];
        size_t dest = gr2->sectorOffsets[i];
        if (dest + sector.decompressLen > gr2->dataSize) {
            free(raw);
            snprintf(log, log_len, "Sector %u is outside the decompressed image.", i);
            return 0;
        }
        if (sector.compressType == COMPRESSION_TYPE_NONE) {
            if ((size_t)sector.dataOffset + sector.decompressLen > flen) {
                free(raw);
                snprintf(log, log_len, "Uncompressed sector %u is outside the file.", i);
                return 0;
            }
            memcpy(raw + dest, file + sector.dataOffset, sector.decompressLen);
        } else if (sector.compressType == COMPRESSION_TYPE_OODLE0 || sector.compressType == COMPRESSION_TYPE_OODLE1) {
            uint32_t extra = (uint32_t)Compression_GetExtraLen(sector.compressType);
            uint8_t *comp, *decomp;
            int ok;
            if ((size_t)sector.dataOffset + sector.compressedLen > flen) {
                free(raw);
                snprintf(log, log_len, "Compressed sector %u is outside the file.", i);
                return 0;
            }
            comp = malloc((size_t)sector.compressedLen + extra);
            decomp = malloc(sector.decompressLen);
            if (!comp || !decomp) {
                free(comp);
                free(decomp);
                free(raw);
                snprintf(log, log_len, "Out of memory.");
                return 0;
            }
            memcpy(comp, file + sector.dataOffset, sector.compressedLen);
            if (extra)
                memset(comp + sector.compressedLen, 0, extra);
            ok = Compression_UnOodle1(comp, sector.compressedLen, decomp, sector.decompressLen, sector.oodleStop0, sector.oodleStop1, gr2->mismatchEndianness);
            if (!ok) {
                free(comp);
                free(decomp);
                free(raw);
                snprintf(log, log_len, "Could not decompress sector %u.", i);
                return 0;
            }
            memcpy(raw + dest, decomp, sector.decompressLen);
            free(comp);
            free(decomp);
        } else {
            free(raw);
            snprintf(log, log_len, "Unsupported compression %u on sector %u.", sector.compressType, i);
            return 0;
        }
    }
    *out = raw;
    return 1;
}

static int write_uncompressed(const char *path, const uint8_t *orig, size_t flen, TGr2 *gr2, const uint8_t *raw, char *log, size_t log_len)
{
    uint32_t i, sc = gr2->fileInfo.sectorCount;
    size_t prefix, cursor, total, fix_bytes, mar_bytes;
    uint8_t *buf;
    TFileInfo *fi;
    TSector *secs;
    FILE *fp;
    prefix = sizeof(THeader) + gr2->fileInfo.fileInfoSize + (size_t)sc * sizeof(TSector);
    if (prefix > flen) {
        snprintf(log, log_len, "FGX header is larger than the file.");
        return 0;
    }
    total = prefix;
    for (i = 0; i < sc; i++) {
        TSector s = gr2->sectors[i];
        total += (size_t)s.fixupSize * sizeof(TFixUpData);
        total += (size_t)s.marshallSize * sizeof(TMarshallData);
        total += s.decompressLen;
    }
    if (total > 0xffffffffu) {
        snprintf(log, log_len, "Rebuilt FGX would exceed 4 GB.");
        return 0;
    }
    buf = calloc(1, total);
    if (!buf) {
        snprintf(log, log_len, "Out of memory.");
        return 0;
    }
    memcpy(buf, orig, prefix);
    secs = (TSector *)(buf + sizeof(THeader) + gr2->fileInfo.fileInfoSize);
    cursor = prefix;
    for (i = 0; i < sc; i++) {
        TSector s = gr2->sectors[i];
        fix_bytes = (size_t)s.fixupSize * sizeof(TFixUpData);
        mar_bytes = (size_t)s.marshallSize * sizeof(TMarshallData);
        if (s.fixupSize) {
            if ((size_t)s.fixupOffset + fix_bytes > flen) {
                free(buf);
                snprintf(log, log_len, "Fixup table for sector %u is outside the file.", i);
                return 0;
            }
            memcpy(buf + cursor, orig + s.fixupOffset, fix_bytes);
            secs[i].fixupOffset = (uint32_t)cursor;
            cursor += fix_bytes;
        }
        if (s.marshallSize) {
            if ((size_t)s.marshallOffset + mar_bytes > flen) {
                free(buf);
                snprintf(log, log_len, "Marshall table for sector %u is outside the file.", i);
                return 0;
            }
            memcpy(buf + cursor, orig + s.marshallOffset, mar_bytes);
            secs[i].marshallOffset = (uint32_t)cursor;
            cursor += mar_bytes;
        }
        memcpy(buf + cursor, raw + gr2->sectorOffsets[i], s.decompressLen);
        secs[i].dataOffset = (uint32_t)cursor;
        secs[i].compressType = COMPRESSION_TYPE_NONE;
        secs[i].compressedLen = s.decompressLen;
        cursor += s.decompressLen;
    }
    fi = (TFileInfo *)(buf + sizeof(THeader));
    fi->totalSize = (uint32_t)total;
    fi->crc32 = CRC32(buf + sizeof(THeader) + fi->fileInfoSize, total - sizeof(THeader) - fi->fileInfoSize);
    fp = fopen(path, "wb");
    if (!fp) {
        free(buf);
        snprintf(log, log_len, "Cannot write %s", path);
        return 0;
    }
    if (fwrite(buf, 1, total, fp) != total) {
        fclose(fp);
        free(buf);
        snprintf(log, log_len, "Short write to %s", path);
        return 0;
    }
    fclose(fp);
    free(buf);
    return 1;
}

static void write_f(uint8_t *p, float v)
{
    memcpy(p, &v, 4);
}

static const Field *uv_field(Field *fields, int nfields, int which)
{
    char a[40], b[40];
    const Field *f;
    snprintf(a, sizeof a, "TextureCoordinates%d", which);
    snprintf(b, sizeof b, "map%d", which + 1);
    f = field_named(fields, nfields, a);
    if (!f)
        f = field_named(fields, nfields, b);
    return f;
}

static void write_vec3(uint8_t *base, const Field *f, const double *n)
{
    int i;
    if (!f)
        return;
    for (i = 0; i < 3 && i < f->count; i++) {
        if (f->type == TYPEID_REAL32)
            write_f(base + f->offset + i * f->length, (float)n[i]);
    }
}

static void write_vec2(uint8_t *base, const Field *f, double u, double v)
{
    if (!f || f->type != TYPEID_REAL32)
        return;
    if (f->count > 0)
        write_f(base + f->offset, (float)u);
    if (f->count > 1)
        write_f(base + f->offset + f->length, (float)v);
}

static int binding_index(TElementGeneric *binds, const char *name)
{
    int i, n;
    if (!binds || !name || !name[0])
        return -1;
    n = group_count(binds);
    for (i = 0; i < n; i++) {
        const char *bn = str_of(group_field(binds, i, "BoneName"));
        if (!bn[0])
            bn = str_of(group_field(binds, i, "Name"));
        if (!bn[0]) {
            TElementGeneric *bb = child_at(binds, i * group_len(binds));
            if (bb && bb->rawInfo.type == TYPEID_STRING)
                bn = str_of(bb);
            else
                bn = str_of(child_named(bb, "Name"));
        }
        if (bn[0] && strcmp(bn, name) == 0)
            return i;
    }
    return -1;
}

typedef struct {
    char name[128];
    int parent;
    float pos[3];
    float rot[4];
    float inv[16];
    int has_xform;
    int has_inv;
} Cn6Bone;

typedef struct {
    char name[128];
    double *nums;
    int nverts;
    int *tris;
    int ntris;
} Cn6Mesh;

typedef struct {
    Cn6Bone *bones;
    int nbones;
    int bones_cap;
    Cn6Mesh *meshes;
    int nmeshes;
    int cap;
} Cn6File;

static void cn6_bone_identity(Cn6Bone *b)
{
    memset(b->pos, 0, sizeof b->pos);
    memset(b->rot, 0, sizeof b->rot);
    b->rot[3] = 1.0f;
    memset(b->inv, 0, sizeof b->inv);
    b->inv[0] = b->inv[5] = b->inv[10] = b->inv[15] = 1.0f;
}

static void cn6_free(Cn6File *c)
{
    int i;
    for (i = 0; i < c->nmeshes; i++) {
        free(c->meshes[i].nums);
        free(c->meshes[i].tris);
    }
    free(c->meshes);
    free(c->bones);
}

static int cn6_add_mesh(Cn6File *c, const char *name)
{
    Cn6Mesh *m;
    if (c->nmeshes == c->cap) {
        int ncap = c->cap ? c->cap * 2 : 4;
        Cn6Mesh *grown = realloc(c->meshes, (size_t)ncap * sizeof(Cn6Mesh));
        if (!grown)
            return 0;
        c->meshes = grown;
        c->cap = ncap;
    }
    m = &c->meshes[c->nmeshes++];
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", name ? name : "");
    return 1;
}

static int parse_cn6(const char *path, Cn6File *out, char *log, size_t log_len)
{
    FILE *fp;
    char line[8192];
    int state = 0, i;
    memset(out, 0, sizeof *out);
    fp = fopen(path, "r");
    if (!fp) {
        snprintf(log, log_len, "Cannot read %s", path);
        return 0;
    }
    out->bones_cap = 1024;
    out->bones = calloc((size_t)out->bones_cap, sizeof(Cn6Bone));
    if (!out->bones) {
        fclose(fp);
        snprintf(log, log_len, "Out of memory.");
        return 0;
    }
    for (i = 0; i < out->bones_cap; i++) {
        out->bones[i].parent = -1;
        cn6_bone_identity(&out->bones[i]);
    }
    while (fgets(line, sizeof line, fp)) {
        char *p = line;
        size_t n;
        while (*p == ' ' || *p == '\t')
            p++;
        n = strlen(p);
        while (n && (p[n - 1] == '\n' || p[n - 1] == '\r'))
            p[--n] = 0;
        if (!p[0] || p[0] == '#' || (p[0] == '/' && p[1] == '/'))
            continue;
        if (!strncmp(p, "skeleton", 8)) {
            state = 1;
            continue;
        }
        if (!strncmp(p, "meshes:", 7) || !strncmp(p, "materials", 9)) {
            state = 0;
            continue;
        }
        if (!strncmp(p, "mesh:", 5)) {
            char name[128];
            const char *q = strchr(p, '"');
            name[0] = 0;
            if (q) {
                const char *e = strchr(q + 1, '"');
                size_t ln = e ? (size_t)(e - (q + 1)) : strlen(q + 1);
                if (ln > sizeof name - 1)
                    ln = sizeof name - 1;
                memcpy(name, q + 1, ln);
                name[ln] = 0;
            }
            if (!cn6_add_mesh(out, name)) {
                fclose(fp);
                cn6_free(out);
                snprintf(log, log_len, "Out of memory.");
                return 0;
            }
            state = 0;
            continue;
        }
        if (!strncmp(p, "vertices", 8)) {
            state = 2;
            continue;
        }
        if (!strncmp(p, "triangles", 9)) {
            state = 3;
            continue;
        }
        if (!strcmp(p, "end"))
            break;
        if (state == 1) {
            int id = 0, parent = -1, nf = 0;
            char name[128];
            const char *q1, *q2, *s;
            char *end = NULL;
            double nums[24];
            name[0] = 0;
            if (sscanf(p, "%d \"%127[^\"]\" %d", &id, name, &parent) < 2 || id < 0 || id >= out->bones_cap)
                continue;
            q1 = strchr(p, '"');
            q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            s = q2 ? q2 + 1 : NULL;
            if (s) {
                while (nf < 24 && *s) {
                    double v;
                    while (*s == ' ' || *s == '\t')
                        s++;
                    if (!*s)
                        break;
                    v = strtod(s, &end);
                    if (end == s)
                        break;
                    s = end;
                    nums[nf++] = v;
                }
            }
            {
                Cn6Bone *b = &out->bones[id];
                snprintf(b->name, sizeof b->name, "%s", name);
                b->parent = nf >= 1 ? (int)nums[0] : parent;
                if (nf >= 8) {
                    b->pos[0] = (float)nums[1];
                    b->pos[1] = (float)nums[2];
                    b->pos[2] = (float)nums[3];
                    b->rot[0] = (float)nums[4];
                    b->rot[1] = (float)nums[5];
                    b->rot[2] = (float)nums[6];
                    b->rot[3] = (float)nums[7];
                    b->has_xform = 1;
                }
                if (nf >= 24) {
                    int k;
                    for (k = 0; k < 16; k++)
                        b->inv[k] = (float)nums[8 + k];
                    b->has_inv = 1;
                }
                if (id + 1 > out->nbones)
                    out->nbones = id + 1;
            }
        } else if (state == 2 && out->nmeshes) {
            Cn6Mesh *m = &out->meshes[out->nmeshes - 1];
            double num[40];
            int got = 0;
            char *s = p, *end;
            double *grown;
            while (got < 40 && *s) {
                while (*s == ' ' || *s == '\t')
                    s++;
                if (!*s)
                    break;
                num[got] = strtod(s, &end);
                if (end == s)
                    break;
                s = end;
                got++;
            }
            if (got < 14)
                continue;
            while (got < 34)
                num[got++] = 0;
            grown = realloc(m->nums, (size_t)(m->nverts + 1) * 34 * sizeof(double));
            if (!grown) {
                fclose(fp);
                cn6_free(out);
                snprintf(log, log_len, "Out of memory.");
                return 0;
            }
            m->nums = grown;
            memcpy(m->nums + (size_t)m->nverts * 34, num, 34 * sizeof(double));
            m->nverts++;
        } else if (state == 3 && out->nmeshes) {
            Cn6Mesh *m = &out->meshes[out->nmeshes - 1];
            int a, b, c, g = 0, *grown;
            if (sscanf(p, "%d %d %d %d", &a, &b, &c, &g) < 3)
                continue;
            grown = realloc(m->tris, (size_t)(m->ntris + 1) * 4 * sizeof(int));
            if (!grown) {
                fclose(fp);
                cn6_free(out);
                snprintf(log, log_len, "Out of memory.");
                return 0;
            }
            m->tris = grown;
            m->tris[m->ntris * 4 + 0] = a;
            m->tris[m->ntris * 4 + 1] = b;
            m->tris[m->ntris * 4 + 2] = c;
            m->tris[m->ntris * 4 + 3] = g;
            m->ntris++;
        }
    }
    fclose(fp);
    if (!out->nmeshes) {
        snprintf(log, log_len, "CN6 has no meshes.");
        cn6_free(out);
        return 0;
    }
    return 1;
}

static uint8_t *array_bytes(TGr2 *gr2, TElementGeneric *e)
{
    uint8_t *p;
    if (!e)
        return NULL;
    p = (uint8_t *)((TElementArray *)e)->data;
    if (p)
        return p;
    if (e->children.count) {
        TElementGeneric *leaf = child_at(e, 0);
        if (leaf && leaf->rawInfo.type == TYPEID_INT16 && ((TElementInt16 *)leaf)->value)
            return (uint8_t *)((TElementInt16 *)leaf)->value;
        if (leaf && leaf->rawInfo.type == TYPEID_INT32 && ((TElementInt32 *)leaf)->value)
            return (uint8_t *)((TElementInt32 *)leaf)->value;
    }
    return NULL;
}

static int ptr_off(TGr2 *gr2, const uint8_t *p, size_t *off)
{
    if (!p || p < gr2->data || p >= gr2->data + gr2->dataSize)
        return 0;
    *off = (size_t)(p - gr2->data);
    return 1;
}

static int import_cn6(TGr2 *gr2, const uint8_t *file, size_t flen, const char *cn6_path, const char *out_path, char *log, size_t log_len)
{
    Cn6File cn6;
    uint8_t *raw = NULL;
    TElementGeneric *meshes, *bones;
    int mi, mesh_count, overwritten = 0, vert_total = 0;
    if (!parse_cn6(cn6_path, &cn6, log, log_len))
        return 0;
    meshes = child_named(gr2->root, "FxsMeshes");
    bones = NULL;
    {
        TElementGeneric *skels = child_named(gr2->root, "FxsSkeletons");
        if (skels)
            bones = group_field(skels, 0, "Bones");
    }
    if (!meshes || group_count(meshes) < 1) {
        cn6_free(&cn6);
        snprintf(log, log_len, "Template FGX has no meshes.");
        return 0;
    }
    mesh_count = group_count(meshes);
    if (cn6.nmeshes != mesh_count) {
        snprintf(log, log_len, "CN6 has %d meshes, template has %d. Counts must match.", cn6.nmeshes, mesh_count);
        cn6_free(&cn6);
        return 0;
    }
    if (!decompress_raw(file, flen, gr2, &raw, log, log_len)) {
        cn6_free(&cn6);
        return 0;
    }
    for (mi = 0; mi < mesh_count; mi++) {
        Cn6Mesh *cm = &cn6.meshes[mi];
        TElementGeneric *vdata_ref, *verts_el, *indices, *binds, *topo_ref;
        TElementArray *verts;
        Field fields[24];
        const Field *fpos, *fnrm, *ftan, *fbin, *fw, *fi;
        int nfields, stride = 0, vcount, vi, nidx, use16;
        const uint8_t *vbytes;
        size_t voff = 0, ioff = 0;
        uint8_t *ibase;
        vdata_ref = group_field(meshes, mi, "PrimaryVertexData");
        if (!vdata_ref)
            vdata_ref = group_field(meshes, mi, "VertexData");
        verts_el = NULL;
        if (vdata_ref && vdata_ref->children.count)
            verts_el = child_named(vdata_ref, "Vertices");
        if (!verts_el) {
            TElementGeneric *vdatas = child_named(gr2->root, "FxsVertexDatas");
            if (vdatas)
                verts_el = group_field(vdatas, mi, "Vertices");
        }
        verts = (TElementArray *)verts_el;
        nfields = verts_el ? decode_fields(gr2, verts, fields, 24, &stride) : 0;
        vcount = verts_el ? (int)verts_el->size : 0;
        vbytes = array_bytes(gr2, verts_el);
        if (!vbytes || stride < 1 || !ptr_off(gr2, vbytes, &voff)) {
            snprintf(log, log_len, "Mesh %d (%s) has no vertex bytes to overwrite.", mi, cm->name);
            free(raw);
            cn6_free(&cn6);
            return 0;
        }
        if (cm->nverts != vcount) {
            snprintf(log, log_len, "Mesh %d (%s) has %d CN6 vertices and %d in the template. This command cannot resize.", mi, cm->name, cm->nverts, vcount);
            free(raw);
            cn6_free(&cn6);
            return 0;
        }
        fpos = field_named(fields, nfields, "Position");
        fnrm = field_named(fields, nfields, "Normal");
        ftan = field_named(fields, nfields, "Tangent");
        fbin = field_named(fields, nfields, "Binormal");
        fw = field_named(fields, nfields, "BoneWeights");
        fi = field_named(fields, nfields, "BoneIndices");
        binds = group_field(meshes, mi, "BoneBindings");
        if (!fpos) {
            snprintf(log, log_len, "Mesh %d has no Position field.", mi);
            free(raw);
            cn6_free(&cn6);
            return 0;
        }
        {
            int span = 0, fi_i;
            for (fi_i = 0; fi_i < nfields; fi_i++) {
                int end = fields[fi_i].offset + fields[fi_i].length * fields[fi_i].count;
                if (end > span)
                    span = end;
            }
            if (span < 1)
                span = stride;
            for (vi = 0; vi < vcount; vi++) {
            const double *num = cm->nums + (size_t)vi * 34;
            uint8_t *base = raw + voff + (size_t)vi * (size_t)stride;
            int z;
            if (voff + (size_t)vi * (size_t)stride + (size_t)span > gr2->dataSize) {
                snprintf(log, log_len, "Mesh %d vertices run past the sector (off %zu stride %d span %d vert %d/%d size %zu).", mi, voff, stride, span, vi, vcount, gr2->dataSize);
                free(raw);
                cn6_free(&cn6);
                return 0;
            }
            write_vec3(base, fpos, num + 0);
            write_vec3(base, fnrm, num + 3);
            write_vec3(base, ftan, num + 6);
            write_vec3(base, fbin, num + 9);
            write_vec2(base, uv_field(fields, nfields, 0), num[12], num[13]);
            write_vec2(base, uv_field(fields, nfields, 1), num[14], num[15]);
            write_vec2(base, uv_field(fields, nfields, 2), num[16], num[17]);
            for (z = 0; fi && z < 8 && z < fi->count; z++) {
                int id = (int)num[18 + z];
                int local = id;
                const char *bname = (id >= 0 && id < cn6.nbones) ? cn6.bones[id].name : "";
                int bound = binding_index(binds, bname);
                uint8_t *bp = base + fi->offset + z * fi->length;
                if (bound >= 0)
                    local = bound;
                if (local < 0)
                    local = 0;
                if (fi->type == TYPEID_UINT8 || fi->type == TYPEID_NORMALUINT8)
                    bp[0] = (uint8_t)(local > 255 ? 255 : local);
                else if (fi->type == TYPEID_UINT16 || fi->type == TYPEID_NORMALUINT16) {
                    bp[0] = (uint8_t)(local & 255);
                    bp[1] = (uint8_t)((local >> 8) & 255);
                } else if (fi->type == TYPEID_INT32) {
                    int32_t v = local;
                    memcpy(bp, &v, 4);
                }
            }
            for (z = 0; fw && z < 8 && z < fw->count; z++) {
                int w = (int)(num[26 + z] + (num[26 + z] >= 0 ? 0.5 : -0.5));
                uint8_t *bp = base + fw->offset + z * fw->length;
                if (w < 0)
                    w = 0;
                if (w > 255)
                    w = 255;
                if (fw->type == TYPEID_UINT8 || fw->type == TYPEID_NORMALUINT8)
                    bp[0] = (uint8_t)w;
                else if (fw->type == TYPEID_REAL32)
                    write_f(bp, (float)w / 255.0f);
            }
            }
        }
        topo_ref = group_field(meshes, mi, "PrimaryTopology");
        indices = NULL;
        if (topo_ref)
            indices = child_named(topo_ref, "Indices16");
        if (!indices && topo_ref)
            indices = child_named(topo_ref, "Indices");
        if (!indices) {
            TElementGeneric *topos = child_named(gr2->root, "FxsTriTopologies");
            if (topos && group_count(topos)) {
                indices = group_field(topos, mi, "Indices16");
                if (!indices)
                    indices = group_field(topos, mi, "Indices");
            }
        }
        nidx = indices ? (int)indices->size : 0;
        use16 = indices && indices->name && strcmp(indices->name, "Indices16") == 0;
        ibase = indices ? array_bytes(gr2, indices) : NULL;
        if (indices && indices->children.count) {
            TElementGeneric *leaf = child_at(indices, 0);
            if (leaf && leaf->rawInfo.type == TYPEID_INT16) {
                use16 = 1;
                if (nidx < 3)
                    nidx = leaf->rawInfo.arraySize > 0 ? leaf->rawInfo.arraySize : (int)leaf->size;
            }
        }
        if (!ibase || !ptr_off(gr2, ibase, &ioff)) {
            snprintf(log, log_len, "Mesh %d has no triangle index bytes.", mi);
            free(raw);
            cn6_free(&cn6);
            return 0;
        }
        if (cm->ntris * 3 != nidx) {
            snprintf(log, log_len, "Mesh %d (%s) has %d CN6 indices and %d in the template. This command cannot resize.", mi, cm->name, cm->ntris * 3, nidx);
            free(raw);
            cn6_free(&cn6);
            return 0;
        }
        for (vi = 0; vi < nidx; vi++) {
            int idx = cm->tris[(vi / 3) * 4 + (vi % 3)];
            if (use16) {
                if (ioff + (size_t)(vi + 1) * 2 > gr2->dataSize) {
                    snprintf(log, log_len, "Mesh %d indices run past the sector.", mi);
                    free(raw);
                    cn6_free(&cn6);
                    return 0;
                }
                raw[ioff + (size_t)vi * 2] = (uint8_t)(idx & 255);
                raw[ioff + (size_t)vi * 2 + 1] = (uint8_t)((idx >> 8) & 255);
            } else {
                int32_t v = idx;
                if (ioff + (size_t)(vi + 1) * 4 > gr2->dataSize) {
                    snprintf(log, log_len, "Mesh %d indices run past the sector.", mi);
                    free(raw);
                    cn6_free(&cn6);
                    return 0;
                }
                memcpy(raw + ioff + (size_t)vi * 4, &v, 4);
            }
        }
        overwritten++;
        vert_total += vcount;
    }
    if (!write_uncompressed(out_path, file, flen, gr2, raw, log, log_len)) {
        free(raw);
        cn6_free(&cn6);
        return 0;
    }
    free(raw);
    cn6_free(&cn6);
    snprintf(log, log_len, "Wrote %s  (overwrote %d meshes, %d vertices)", out_path, overwritten, vert_total);
    return 1;
}

static size_t align_up(size_t v, size_t a)
{
    if (a < 2)
        return v;
    return (v + a - 1) & ~(a - 1);
}

static int sector_of(TGr2 *gr2, size_t off, size_t *local)
{
    uint32_t i;
    for (i = 0; i < gr2->fileInfo.sectorCount; i++) {
        size_t start = gr2->sectorOffsets[i];
        size_t len = gr2->sectors[i].decompressLen;
        if (len && off >= start && off < start + len) {
            *local = off - start;
            return (int)i;
        }
    }
    return -1;
}

static int decompress_one(const uint8_t *file, size_t flen, TGr2 *gr2, uint32_t i, uint8_t **out, char *log, size_t log_len)
{
    TSector sector = gr2->sectors[i];
    uint8_t *raw;
    if (!sector.decompressLen) {
        *out = NULL;
        return 1;
    }
    raw = calloc(1, sector.decompressLen);
    if (!raw) {
        snprintf(log, log_len, "Out of memory.");
        return 0;
    }
    if (sector.compressType == COMPRESSION_TYPE_NONE) {
        if ((size_t)sector.dataOffset + sector.decompressLen > flen) {
            free(raw);
            snprintf(log, log_len, "Sector %u is outside the file.", i);
            return 0;
        }
        memcpy(raw, file + sector.dataOffset, sector.decompressLen);
    } else if (sector.compressType == COMPRESSION_TYPE_OODLE0 || sector.compressType == COMPRESSION_TYPE_OODLE1) {
        uint32_t extra = (uint32_t)Compression_GetExtraLen(sector.compressType);
        uint8_t *comp = malloc((size_t)sector.compressedLen + extra);
        uint8_t *decomp = malloc(sector.decompressLen);
        int ok;
        if (!comp || !decomp || (size_t)sector.dataOffset + sector.compressedLen > flen) {
            free(comp);
            free(decomp);
            free(raw);
            snprintf(log, log_len, "Could not decompress sector %u.", i);
            return 0;
        }
        memcpy(comp, file + sector.dataOffset, sector.compressedLen);
        if (extra)
            memset(comp + sector.compressedLen, 0, extra);
        ok = Compression_UnOodle1(comp, sector.compressedLen, decomp, sector.decompressLen, sector.oodleStop0, sector.oodleStop1, gr2->mismatchEndianness);
        free(comp);
        if (!ok) {
            free(decomp);
            free(raw);
            snprintf(log, log_len, "Could not decompress sector %u.", i);
            return 0;
        }
        memcpy(raw, decomp, sector.decompressLen);
        free(decomp);
    } else {
        free(raw);
        snprintf(log, log_len, "Unsupported compression on sector %u.", i);
        return 0;
    }
    *out = raw;
    return 1;
}

static int find_template(const Cn6File *cn6, char *path, size_t n, int *skinned)
{
    int m, v, skin = 0, uv3 = 0;
    char exe[1024], base[1024];
    ssize_t k;
    const char *which;
    for (m = 0; m < cn6->nmeshes; m++) {
        for (v = 0; v < cn6->meshes[m].nverts; v++) {
            const double *num = cn6->meshes[m].nums + (size_t)v * 34;
            if ((int)num[18] > 0 || (int)num[19] > 0 || (int)num[27] > 0)
                skin = 1;
            if (num[16] > 0.001 || num[16] < -0.001 || (num[17] > 0.001 && (num[17] < 0.999 || num[17] > 1.001)))
                uv3 = 1;
        }
    }
    *skinned = skin;
    which = skin ? "model_template.fgx" : (uv3 ? "model_template_3uv.fgx" : "model_template_2uv.fgx");
    k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k > 0) {
        char *slash;
        exe[k] = 0;
        slash = strrchr(exe, '/');
        if (slash)
            *slash = 0;
        snprintf(path, n, "%s/../share/civnexus6/%s", exe, which);
        if (access(path, 0) == 0)
            return 1;
    }
    snprintf(base, sizeof base, "/tmp/AppDir/usr/share/civnexus6/%s", which);
    if (access(base, 0) == 0) {
        snprintf(path, n, "%s", base);
        return 1;
    }
    snprintf(path, n, "%s", which);
    return access(path, 0) == 0;
}

static void write_uvs(uint8_t *base, const Field *f, const double *uv, int pairs)
{
    int i;
    if (!f || f->type != TYPEID_REAL32)
        return;
    for (i = 0; i < pairs * 2 && i < f->count; i++)
        write_f(base + f->offset + i * f->length, (float)uv[i]);
}

/* 64-bit Civ VI bone record. Pointer slots are filled by fixups, not by the bytes. */
#define CN6_BONE_STRIDE 164
#define CN6_BIND_STRIDE 44

typedef struct {
    TFixUpData *fd;
    uint32_t n;
    uint32_t cap;
} FixList;

static int fix_load(FixList *fl, const uint8_t *file, TSector sec)
{
    fl->n = sec.fixupSize;
    fl->cap = sec.fixupSize + 64;
    fl->fd = fl->cap ? calloc(fl->cap, sizeof(TFixUpData)) : NULL;
    if (fl->cap && !fl->fd)
        return 0;
    if (sec.fixupSize)
        memcpy(fl->fd, file + sec.fixupOffset, (size_t)sec.fixupSize * sizeof(TFixUpData));
    return 1;
}

static int fix_add(FixList *fl, uint32_t src, uint32_t dst_sec, uint32_t dst)
{
    if (fl->n + 1 > fl->cap) {
        uint32_t ncap = fl->cap ? fl->cap * 2 : 16;
        TFixUpData *grown = realloc(fl->fd, (size_t)ncap * sizeof(TFixUpData));
        if (!grown)
            return 0;
        fl->fd = grown;
        fl->cap = ncap;
    }
    fl->fd[fl->n].srcOffset = src;
    fl->fd[fl->n].dstSector = dst_sec;
    fl->fd[fl->n].dstOffset = dst;
    fl->n++;
    return 1;
}

static void fix_shift(FixList *fl, uint32_t nsec, uint32_t sec, uint32_t at, uint32_t extra)
{
    uint32_t s, i;
    if (!extra)
        return;
    for (s = 0; s < nsec; s++) {
        for (i = 0; i < fl[s].n; i++) {
            if (s == sec && fl[s].fd[i].srcOffset >= at)
                fl[s].fd[i].srcOffset += extra;
            if (fl[s].fd[i].dstSector == sec && fl[s].fd[i].dstOffset >= at)
                fl[s].fd[i].dstOffset += extra;
        }
    }
}

static TFixUpData *fix_find_src(FixList *fl, uint32_t src)
{
    uint32_t i;
    for (i = 0; i < fl->n; i++)
        if (fl->fd[i].srcOffset == src)
            return &fl->fd[i];
    return NULL;
}

static TFixUpData *fix_find_dst(FixList *fl, uint32_t nsec, uint32_t dst_sec, uint32_t dst, uint32_t *src_sec)
{
    uint32_t s, i;
    for (s = 0; s < nsec; s++) {
        for (i = 0; i < fl[s].n; i++) {
            if (fl[s].fd[i].dstSector == dst_sec && fl[s].fd[i].dstOffset == dst) {
                if (src_sec)
                    *src_sec = s;
                return &fl[s].fd[i];
            }
        }
    }
    return NULL;
}

static int sec_insert(uint8_t **p, uint32_t *len, uint32_t at, uint32_t extra)
{
    uint8_t *n;
    if (!extra)
        return 1;
    if (at > *len)
        return 0;
    n = calloc(1, (size_t)*len + extra);
    if (!n)
        return 0;
    if (at)
        memcpy(n, *p, at);
    if (*len > at)
        memcpy(n + at + extra, *p + at, *len - at);
    free(*p);
    *p = n;
    *len += extra;
    return 1;
}

static uint32_t sec_append_str(uint8_t **p, uint32_t *len, const char *s)
{
    uint32_t at = *len;
    uint32_t n = (uint32_t)strlen(s) + 1;
    if (!sec_insert(p, len, at, n))
        return 0xffffffffu;
    memcpy(*p + at, s, n);
    return at;
}

static int elem_local(TGr2 *gr2, const void *p, int *sec, uint32_t *local)
{
    size_t off, loc;
    int s;
    if (!p || !ptr_off(gr2, (const uint8_t *)p, &off))
        return 0;
    s = sector_of(gr2, off, &loc);
    if (s < 0)
        return 0;
    *sec = s;
    *local = (uint32_t)loc;
    return 1;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static void put_i32(uint8_t *p, int32_t v)
{
    memcpy(p, &v, 4);
}

static int near0f(float v)
{
    return v > -0.0001f && v < 0.0001f;
}

static void write_bone_rec(uint8_t *rec, const uint8_t *proto, const Cn6Bone *b)
{
    int i;
    uint32_t flags = 0;
    memcpy(rec, proto, CN6_BONE_STRIDE);
    memset(rec, 0, 8);
    memset(rec + 148, 0, 16);
    put_i32(rec + 8, b->parent);
    if (!near0f(b->pos[0]) || !near0f(b->pos[1]) || !near0f(b->pos[2]))
        flags |= 1;
    if (!near0f(b->rot[0]) || !near0f(b->rot[1]) || !near0f(b->rot[2]) || b->rot[3] < 0.9999f || b->rot[3] > 1.0001f)
        flags |= 2;
    put_u32(rec + 12, flags);
    for (i = 0; i < 3; i++)
        write_f(rec + 16 + (size_t)i * 4, b->pos[i]);
    for (i = 0; i < 4; i++)
        write_f(rec + 28 + (size_t)i * 4, b->rot[i]);
    if (b->has_inv) {
        for (i = 0; i < 16; i++)
            write_f(rec + 80 + (size_t)i * 4, b->inv[i]);
    }
}

static int install_cn6_skeleton(TGr2 *gr2, uint8_t **secs, uint32_t *seclen, FixList *fl, const Cn6File *cn6, char *log, size_t log_len)
{
    TElementGeneric *skels, *bones, *meshes, *binds;
    TElementGeneric *parent, *inv, *obb;
    int bone_sec = -1, bind_sec = -1, obb_sec = -1;
    uint32_t bone_local = 0, bind_local = 0, parent_local = 0, inv_local = 0, obb_local = 0;
    uint32_t nsec, bone_ptr_src, count_at, name_src, bind_ptr_src, bind_count_at;
    uint32_t old_bones, old_binds, bone_at, bind_at, bone_extra = 0, bind_extra = 0;
    uint32_t src_sec = 0, align, i;
    uint8_t proto[CN6_BONE_STRIDE];
    char skel_name[128];
    const char *mesh_name;
    TFixUpData *bone_ptr, *bind_ptr;
    if (!cn6->nbones)
        return 1;
    if (gr2->bitsSize != 64) {
        snprintf(log, log_len, "Template skeleton is not 64-bit, so the CN6 bones cannot be written.");
        return 0;
    }
    skels = child_named(gr2->root, "FxsSkeletons");
    meshes = child_named(gr2->root, "FxsMeshes");
    bones = skels ? group_field(skels, 0, "Bones") : NULL;
    binds = meshes ? group_field(meshes, 0, "BoneBindings") : NULL;
    if (!bones || !bones->size || !((TElementArray *)bones)->data) {
        snprintf(log, log_len, "Template has no skeleton to extend.");
        return 0;
    }
    if (!binds || !binds->size || !((TElementArray *)binds)->data) {
        snprintf(log, log_len, "Template has no bone bindings to extend.");
        return 0;
    }
    if (!elem_local(gr2, ((TElementArray *)bones)->data, &bone_sec, &bone_local) || !secs[bone_sec]) {
        snprintf(log, log_len, "Template bone array is not inside a sector.");
        return 0;
    }
    if (!elem_local(gr2, ((TElementArray *)binds)->data, &bind_sec, &bind_local) || !secs[bind_sec]) {
        snprintf(log, log_len, "Template bone bindings are not inside a sector.");
        return 0;
    }
    parent = group_field(bones, 0, "ParentIndex");
    inv = group_field(bones, 0, "InverseWorldTransform");
    obb = group_field(binds, 0, "OBBMin");
    {
        int psec = -1, isec2 = -1;
        if (!parent || parent->rawInfo.type != TYPEID_INT32 || !elem_local(gr2, ((TElementInt32 *)parent)->value, &psec, &parent_local) || psec != bone_sec || parent_local != bone_local + 8 ||
            !inv || inv->rawInfo.type != TYPEID_REAL32 || !elem_local(gr2, ((TElementFloat *)inv)->value, &isec2, &inv_local) || isec2 != bone_sec || inv_local != bone_local + 80 ||
            !obb || !elem_local(gr2, ((TElementFloat *)obb)->value, &obb_sec, &obb_local) || obb_sec != bind_sec || obb_local != bind_local + 8) {
            snprintf(log, log_len, "Template bone layout is not the 64-bit Civ VI skeleton.");
            return 0;
        }
    }
    if (bone_local + CN6_BONE_STRIDE > seclen[bone_sec] || bind_local + CN6_BIND_STRIDE > seclen[bind_sec]) {
        snprintf(log, log_len, "Template skeleton is truncated.");
        return 0;
    }
    nsec = gr2->fileInfo.sectorCount;
    bone_ptr = fix_find_dst(fl, nsec, (uint32_t)bone_sec, bone_local, &src_sec);
    if (!bone_ptr || src_sec != (uint32_t)bone_sec || bone_ptr->srcOffset < 12) {
        snprintf(log, log_len, "Template is missing the skeleton pointer.");
        return 0;
    }
    bone_ptr_src = bone_ptr->srcOffset;
    count_at = bone_ptr_src - 4;
    name_src = count_at - 8;
    memcpy(&old_bones, secs[bone_sec] + count_at, 4);
    if (old_bones != bones->size) {
        snprintf(log, log_len, "Template bone count does not match the skeleton.");
        return 0;
    }
    if (!fix_find_src(&fl[bone_sec], name_src) || !fix_find_src(&fl[bone_sec], bone_local)) {
        snprintf(log, log_len, "Template is missing skeleton name fixups.");
        return 0;
    }
    bind_ptr = fix_find_dst(fl, nsec, (uint32_t)bind_sec, bind_local, &src_sec);
    if (!bind_ptr || src_sec != (uint32_t)bind_sec || bind_ptr->srcOffset < 4) {
        snprintf(log, log_len, "Template is missing the bone-binding pointer.");
        return 0;
    }
    bind_ptr_src = bind_ptr->srcOffset;
    bind_count_at = bind_ptr_src - 4;
    memcpy(&old_binds, secs[bind_sec] + bind_count_at, 4);
    if (old_binds != binds->size) {
        snprintf(log, log_len, "Template binding count does not match the mesh.");
        return 0;
    }
    if (bind_local + old_binds * CN6_BIND_STRIDE > seclen[bind_sec]) {
        snprintf(log, log_len, "Template bone bindings are truncated.");
        return 0;
    }
    memcpy(proto, secs[bone_sec] + bone_local, CN6_BONE_STRIDE);
    bone_at = bone_local + old_bones * CN6_BONE_STRIDE;
    if ((uint32_t)cn6->nbones > old_bones) {
        bone_extra = ((uint32_t)cn6->nbones - old_bones) * CN6_BONE_STRIDE;
        if (!sec_insert(&secs[bone_sec], &seclen[bone_sec], bone_at, bone_extra)) {
            snprintf(log, log_len, "Out of memory.");
            return 0;
        }
        fix_shift(fl, nsec, (uint32_t)bone_sec, bone_at, bone_extra);
        if (count_at >= bone_at)
            count_at += bone_extra;
        if (name_src >= bone_at)
            name_src += bone_extra;
    }
    bind_at = bind_local;
    if (bind_sec == bone_sec && bind_at >= bone_at)
        bind_at += bone_extra;
    if (bind_count_at >= bone_at && bind_sec == bone_sec)
        bind_count_at += bone_extra;
    if ((uint32_t)cn6->nbones > old_binds) {
        uint32_t bind_insert = bind_at + old_binds * CN6_BIND_STRIDE;
        bind_extra = ((uint32_t)cn6->nbones - old_binds) * CN6_BIND_STRIDE;
        if (!sec_insert(&secs[bind_sec], &seclen[bind_sec], bind_insert, bind_extra)) {
            snprintf(log, log_len, "Out of memory.");
            return 0;
        }
        fix_shift(fl, nsec, (uint32_t)bind_sec, bind_insert, bind_extra);
        if (bind_sec == bone_sec) {
            if (bone_local >= bind_insert)
                bone_local += bind_extra;
            if (count_at >= bind_insert)
                count_at += bind_extra;
            if (name_src >= bind_insert)
                name_src += bind_extra;
        }
    }
    if (bone_local + (uint32_t)cn6->nbones * CN6_BONE_STRIDE > seclen[bone_sec] ||
        bind_at + (uint32_t)cn6->nbones * CN6_BIND_STRIDE > seclen[bind_sec]) {
        snprintf(log, log_len, "Skeleton rewrite ran past the sector.");
        return 0;
    }
    for (i = 0; i < (uint32_t)cn6->nbones; i++) {
        Cn6Bone b = cn6->bones[i];
        if (!b.name[0])
            snprintf(b.name, sizeof b.name, "Bone%u", i);
        if (!b.has_xform)
            cn6_bone_identity(&b);
        write_bone_rec(secs[bone_sec] + bone_local + i * CN6_BONE_STRIDE, proto, &b);
        memset(secs[bind_sec] + bind_at + i * CN6_BIND_STRIDE, 0, CN6_BIND_STRIDE);
    }
    put_u32(secs[bone_sec] + count_at, (uint32_t)cn6->nbones);
    put_u32(secs[bind_sec] + bind_count_at, (uint32_t)cn6->nbones);
    mesh_name = cn6->nmeshes ? cn6->meshes[0].name : "";
    if (mesh_name[0] && strcmp(mesh_name, "BLANK_MESH") && strcmp(mesh_name, "BLANK_SKELETON"))
        snprintf(skel_name, sizeof skel_name, "%s", mesh_name);
    else if (cn6->bones[0].name[0])
        snprintf(skel_name, sizeof skel_name, "%s", cn6->bones[0].name);
    else
        snprintf(skel_name, sizeof skel_name, "Skeleton");
    {
        uint32_t skel_at = sec_append_str(&secs[bone_sec], &seclen[bone_sec], skel_name);
        TFixUpData *nm;
        if (skel_at == 0xffffffffu) {
            snprintf(log, log_len, "Out of memory.");
            return 0;
        }
        nm = fix_find_src(&fl[bone_sec], name_src);
        if (!nm) {
            snprintf(log, log_len, "Lost the skeleton name while writing bones.");
            return 0;
        }
        nm->dstSector = (uint32_t)bone_sec;
        nm->dstOffset = skel_at;
        for (i = 0; i < (uint32_t)cn6->nbones; i++) {
            char bname[128];
            uint32_t at, src;
            TFixUpData *slot;
            snprintf(bname, sizeof bname, "%s", cn6->bones[i].name[0] ? cn6->bones[i].name : "Bone");
            if (!cn6->bones[i].name[0])
                snprintf(bname, sizeof bname, "Bone%u", i);
            at = sec_append_str(&secs[bone_sec], &seclen[bone_sec], bname);
            if (at == 0xffffffffu) {
                snprintf(log, log_len, "Out of memory.");
                return 0;
            }
            src = bone_local + i * CN6_BONE_STRIDE;
            slot = fix_find_src(&fl[bone_sec], src);
            if (slot) {
                slot->dstSector = (uint32_t)bone_sec;
                slot->dstOffset = at;
            } else if (!fix_add(&fl[bone_sec], src, (uint32_t)bone_sec, at)) {
                snprintf(log, log_len, "Out of memory.");
                return 0;
            }
            src = bind_at + i * CN6_BIND_STRIDE;
            at = sec_append_str(&secs[bind_sec == bone_sec ? bone_sec : bone_sec], &seclen[bone_sec], bname);
            if (at == 0xffffffffu) {
                snprintf(log, log_len, "Out of memory.");
                return 0;
            }
            slot = fix_find_src(&fl[bind_sec], src);
            if (slot) {
                slot->dstSector = (uint32_t)bone_sec;
                slot->dstOffset = at;
            } else if (!fix_add(&fl[bind_sec], src, (uint32_t)bone_sec, at)) {
                snprintf(log, log_len, "Out of memory.");
                return 0;
            }
        }
    }
    align = gr2->sectors[bone_sec].alignment ? gr2->sectors[bone_sec].alignment : 4;
    if (seclen[bone_sec] % align) {
        if (!sec_insert(&secs[bone_sec], &seclen[bone_sec], seclen[bone_sec], align - (seclen[bone_sec] % align))) {
            snprintf(log, log_len, "Out of memory.");
            return 0;
        }
    }
    if (bind_sec != bone_sec) {
        align = gr2->sectors[bind_sec].alignment ? gr2->sectors[bind_sec].alignment : 4;
        if (seclen[bind_sec] % align) {
            if (!sec_insert(&secs[bind_sec], &seclen[bind_sec], seclen[bind_sec], align - (seclen[bind_sec] % align))) {
                snprintf(log, log_len, "Out of memory.");
                return 0;
            }
        }
    }
    (void)bind_extra;
    return 1;
}

static int create_cn6(const char *cn6_path, const char *out_path, char *log, size_t log_len)
{
    Cn6File cn6;
    char template_path[1024];
    TGr2 gr2;
    uint8_t *file = NULL;
    uint8_t **secs = NULL;
    uint32_t *seclen = NULL;
    FILE *fp;
    long flen = 0;
    int skinned = 0, mi, total_v = 0, total_i = 0, vbase, i, ok = 0;
    TElementGeneric *meshes, *vdata, *verts_el, *topo, *indices, *groups, *tricount;
    Field fields[24];
    const Field *fpos, *fnrm, *ftan, *fbin, *fw, *fi;
    int nfields, stride = 0, old_v = 0, old_i = 0, index_size = 2;
    size_t voff = 0, ioff = 0, vlocal = 0, ilocal = 0, tlocal = 0;
    int vsec = -1, isec = -1, tsec = -1;
    uint8_t *newv = NULL, *newi = NULL, *pattern = NULL;
    size_t new_vlen, new_ilen, align;
    FixList *fl = NULL;
    int wrote_bones = 0;
    if (!parse_cn6(cn6_path, &cn6, log, log_len))
        return 0;
    for (mi = 0; mi < cn6.nmeshes; mi++) {
        total_v += cn6.meshes[mi].nverts;
        total_i += cn6.meshes[mi].ntris * 3;
    }
    if (total_v < 1 || total_i < 3) {
        snprintf(log, log_len, "CN6 needs at least 1 vertex and 1 triangle.");
        cn6_free(&cn6);
        return 0;
    }
    if (total_i > 65535 * 3) {
        snprintf(log, log_len, "Too many triangles for a 16-bit index buffer.");
        cn6_free(&cn6);
        return 0;
    }
    if (!find_template(&cn6, template_path, sizeof template_path, &skinned)) {
        snprintf(log, log_len, "Cannot find the bundled blank template.");
        cn6_free(&cn6);
        return 0;
    }
    fp = fopen(template_path, "rb");
    if (!fp) {
        snprintf(log, log_len, "Cannot read template %s", template_path);
        cn6_free(&cn6);
        return 0;
    }
    fseek(fp, 0, SEEK_END);
    flen = ftell(fp);
    rewind(fp);
    file = malloc((size_t)flen);
    if (!file || fread(file, (size_t)flen, 1, fp) != 1) {
        fclose(fp);
        free(file);
        cn6_free(&cn6);
        snprintf(log, log_len, "Cannot read template %s (%ld bytes)", template_path, flen);
        return 0;
    }
    fclose(fp);
    if (!Gr2_Init(&gr2) || !Gr2_Load(file, (size_t)flen, &gr2)) {
        free(file);
        cn6_free(&cn6);
        snprintf(log, log_len, "Template is not a readable FGX.");
        return 0;
    }
    meshes = child_named(gr2.root, "FxsMeshes");
    vdata = meshes ? group_field(meshes, 0, "PrimaryVertexData") : NULL;
    verts_el = vdata ? child_named(vdata, "Vertices") : NULL;
    topo = meshes ? group_field(meshes, 0, "PrimaryTopology") : NULL;
    indices = topo ? child_named(topo, "Indices16") : NULL;
    if (!indices && topo)
        indices = child_named(topo, "Indices");
    groups = topo ? child_named(topo, "Groups") : NULL;
    tricount = groups ? group_field(groups, 0, "TriCount") : NULL;
    nfields = verts_el ? decode_fields(&gr2, (TElementArray *)verts_el, fields, 24, &stride) : 0;
    old_v = verts_el ? (int)verts_el->size : 0;
    old_i = indices ? (int)indices->size : 0;
    if (indices && indices->name && !strstr(indices->name, "16"))
        index_size = 4;
    if (!verts_el || stride < 12 || old_v < 1 || !array_bytes(&gr2, verts_el) || !ptr_off(&gr2, array_bytes(&gr2, verts_el), &voff)) {
        snprintf(log, log_len, "Template has no vertex array to grow.");
        goto done;
    }
    if (!indices || !array_bytes(&gr2, indices) || !ptr_off(&gr2, array_bytes(&gr2, indices), &ioff)) {
        snprintf(log, log_len, "Template has no index array to grow.");
        goto done;
    }
    vsec = sector_of(&gr2, voff, &vlocal);
    isec = sector_of(&gr2, ioff, &ilocal);
    if (vsec < 0 || isec < 0) {
        snprintf(log, log_len, "Vertex data is not inside a sector.");
        goto done;
    }
    fpos = field_named(fields, nfields, "Position");
    fnrm = field_named(fields, nfields, "Normal");
    ftan = field_named(fields, nfields, "Tangent");
    fbin = field_named(fields, nfields, "Binormal");
    fw = field_named(fields, nfields, "BoneWeights");
    fi = field_named(fields, nfields, "BoneIndices");
    if (!fpos) {
        snprintf(log, log_len, "Template vertex has no Position.");
        goto done;
    }
    secs = calloc(gr2.fileInfo.sectorCount, sizeof *secs);
    seclen = calloc(gr2.fileInfo.sectorCount, sizeof *seclen);
    if (!secs || !seclen) {
        snprintf(log, log_len, "Out of memory.");
        goto done;
    }
    for (i = 0; i < (int)gr2.fileInfo.sectorCount; i++) {
        seclen[i] = gr2.sectors[i].decompressLen;
        if (!decompress_one(file, (size_t)flen, &gr2, (uint32_t)i, &secs[i], log, log_len))
            goto done;
    }
    pattern = malloc((size_t)stride);
    if (!pattern || vlocal + (size_t)stride > seclen[vsec]) {
        snprintf(log, log_len, "Template vertex is outside its sector.");
        goto done;
    }
    memcpy(pattern, secs[vsec] + vlocal, (size_t)stride);
    align = gr2.sectors[vsec].alignment ? gr2.sectors[vsec].alignment : 4;
    new_vlen = align_up(vlocal + (size_t)total_v * (size_t)stride, align);
    newv = calloc(1, new_vlen ? new_vlen : 1);
    if (!newv) {
        snprintf(log, log_len, "Out of memory.");
        goto done;
    }
    if (vlocal)
        memcpy(newv, secs[vsec], vlocal);
    vbase = 0;
    for (mi = 0; mi < cn6.nmeshes; mi++) {
        int vi;
        for (vi = 0; vi < cn6.meshes[mi].nverts; vi++) {
            const double *num = cn6.meshes[mi].nums + (size_t)vi * 34;
            uint8_t *base = newv + vlocal + (size_t)(vbase + vi) * (size_t)stride;
            int z;
            memcpy(base, pattern, (size_t)stride);
            write_vec3(base, fpos, num + 0);
            write_vec3(base, fnrm, num + 3);
            write_vec3(base, ftan, num + 6);
            write_vec3(base, fbin, num + 9);
            write_uvs(base, uv_field(fields, nfields, 0), num + 12, 1);
            write_uvs(base, uv_field(fields, nfields, 1), num + 14, 1);
            write_uvs(base, uv_field(fields, nfields, 2), num + 16, 1);
            if (cn6.nbones > 0) {
                for (z = 0; fi && z < fi->count && z < 8; z++) {
                    int id = (int)num[18 + z];
                    int local = (id >= 0 && id < cn6.nbones) ? id : 0;
                    uint8_t *bp = base + fi->offset + z * fi->length;
                    if ((fi->type == TYPEID_UINT8 || fi->type == TYPEID_NORMALUINT8 || fi->type == TYPEID_INT8) && local > 255) {
                        snprintf(log, log_len, "Bone index %d does not fit in this template.", local);
                        goto done;
                    }
                    if (fi->type == TYPEID_UINT8 || fi->type == TYPEID_NORMALUINT8 || fi->type == TYPEID_INT8)
                        bp[0] = (uint8_t)local;
                    else if (fi->type == TYPEID_UINT16 || fi->type == TYPEID_NORMALUINT16) {
                        bp[0] = (uint8_t)(local & 255);
                        bp[1] = (uint8_t)((local >> 8) & 255);
                    } else if (fi->type == TYPEID_INT32) {
                        int32_t v = local;
                        memcpy(bp, &v, 4);
                    }
                }
            }
            for (z = 0; fw && z < fw->count && z < 8; z++) {
                int w = (int)(num[26 + z] + (num[26 + z] >= 0 ? 0.5 : -0.5));
                uint8_t *bp = base + fw->offset + z * fw->length;
                if (w < 0)
                    w = 0;
                if (w > 255)
                    w = 255;
                if (fw->type == TYPEID_UINT8 || fw->type == TYPEID_NORMALUINT8)
                    bp[0] = (uint8_t)w;
                else if (fw->type == TYPEID_REAL32)
                    write_f(bp, (float)w / 255.0f);
            }
        }
        vbase += cn6.meshes[mi].nverts;
    }
    align = gr2.sectors[isec].alignment ? gr2.sectors[isec].alignment : 4;
    new_ilen = align_up(ilocal + (size_t)total_i * (size_t)index_size, align);
    newi = calloc(1, new_ilen ? new_ilen : 1);
    if (!newi) {
        snprintf(log, log_len, "Out of memory.");
        goto done;
    }
    if (ilocal)
        memcpy(newi, secs[isec], ilocal);
    {
        int cursor = 0;
        vbase = 0;
        for (mi = 0; mi < cn6.nmeshes; mi++) {
            int ti;
            for (ti = 0; ti < cn6.meshes[mi].ntris; ti++) {
                int k;
                for (k = 0; k < 3; k++) {
                    int idx = cn6.meshes[mi].tris[ti * 4 + k] + vbase;
                    uint8_t *bp = newi + ilocal + (size_t)cursor * (size_t)index_size;
                    if (idx < 0)
                        idx = 0;
                    if (index_size == 2) {
                        bp[0] = (uint8_t)(idx & 255);
                        bp[1] = (uint8_t)((idx >> 8) & 255);
                    } else {
                        int32_t v = idx;
                        memcpy(bp, &v, 4);
                    }
                    cursor++;
                }
            }
            vbase += cn6.meshes[mi].nverts;
        }
    }
    /* counts sit in the 4 bytes before the array pointer fixup */
    {
        uint32_t s, k;
        int found_v = 0, found_i = 0;
        for (s = 0; s < gr2.fileInfo.sectorCount; s++) {
            TSector sec = gr2.sectors[s];
            for (k = 0; k < sec.fixupSize; k++) {
                TFixUpData fd;
                size_t dest;
                memcpy(&fd, file + sec.fixupOffset + k * sizeof fd, sizeof fd);
                if (fd.dstSector >= gr2.fileInfo.sectorCount)
                    continue;
                dest = gr2.sectorOffsets[fd.dstSector] + fd.dstOffset;
                if (dest == voff && fd.srcOffset >= 4 && secs[s] && fd.srcOffset - 4 + 4 <= seclen[s]) {
                    uint32_t c = (uint32_t)total_v;
                    memcpy(secs[s] + fd.srcOffset - 4, &c, 4);
                    found_v = 1;
                }
                if (dest == ioff && fd.srcOffset >= 4 && secs[s] && fd.srcOffset - 4 + 4 <= seclen[s]) {
                    uint32_t c = (uint32_t)total_i;
                    memcpy(secs[s] + fd.srcOffset - 4, &c, 4);
                    found_i = 1;
                }
            }
        }
        if (!found_v || !found_i) {
            snprintf(log, log_len, "Could not find the vertex or index count in the template.");
            goto done;
        }
    }
    if (tricount && tricount->rawInfo.type == TYPEID_INT32 && ((TElementInt32 *)tricount)->value) {
        size_t off = (size_t)((uint8_t *)((TElementInt32 *)tricount)->value - gr2.data);
        int32_t c = total_i / 3;
        tsec = sector_of(&gr2, off, &tlocal);
        if (tsec >= 0 && secs[tsec] && tlocal + 4 <= seclen[tsec])
            memcpy(secs[tsec] + tlocal, &c, 4);
    }
    free(secs[vsec]);
    secs[vsec] = newv;
    seclen[vsec] = (uint32_t)new_vlen;
    newv = NULL;
    free(secs[isec]);
    secs[isec] = newi;
    seclen[isec] = (uint32_t)new_ilen;
    newi = NULL;
    fl = calloc(gr2.fileInfo.sectorCount, sizeof *fl);
    if (!fl) {
        snprintf(log, log_len, "Out of memory.");
        goto done;
    }
    for (i = 0; i < (int)gr2.fileInfo.sectorCount; i++) {
        if (!fix_load(&fl[i], file, gr2.sectors[i])) {
            snprintf(log, log_len, "Out of memory.");
            goto done;
        }
    }
    if (cn6.nbones > 0) {
        if (!install_cn6_skeleton(&gr2, secs, seclen, fl, &cn6, log, log_len))
            goto done;
        wrote_bones = cn6.nbones;
    }
    {
        uint32_t sc = gr2.fileInfo.sectorCount, s;
        size_t old_end_v = vlocal + (size_t)old_v * (size_t)stride;
        size_t old_end_i = ilocal + (size_t)old_i * (size_t)index_size;
        long dv = (long)total_v * stride - (long)old_v * stride;
        long di = (long)total_i * index_size - (long)old_i * index_size;
        for (s = 0; s < sc; s++) {
            uint32_t k;
            for (k = 0; k < fl[s].n; k++) {
                if (fl[s].fd[k].dstSector == (uint32_t)vsec && fl[s].fd[k].dstOffset >= old_end_v)
                    fl[s].fd[k].dstOffset = (uint32_t)(fl[s].fd[k].dstOffset + dv);
                if (fl[s].fd[k].dstSector == (uint32_t)isec && fl[s].fd[k].dstOffset >= old_end_i)
                    fl[s].fd[k].dstOffset = (uint32_t)(fl[s].fd[k].dstOffset + di);
            }
        }
    }
    {
        uint32_t sc = gr2.fileInfo.sectorCount, s;
        size_t prefix = sizeof(THeader) + gr2.fileInfo.fileInfoSize + (size_t)sc * sizeof(TSector);
        size_t cursor, total;
        uint8_t *buf;
        TFileInfo *info;
        TSector *outsecs;
        FILE *out;
        total = prefix;
        for (s = 0; s < sc; s++) {
            total += (size_t)fl[s].n * sizeof(TFixUpData);
            total += (size_t)gr2.sectors[s].marshallSize * sizeof(TMarshallData);
            total += seclen[s];
        }
        buf = calloc(1, total);
        if (!buf) {
            snprintf(log, log_len, "Out of memory.");
            goto done;
        }
        memcpy(buf, file, prefix);
        outsecs = (TSector *)(buf + sizeof(THeader) + gr2.fileInfo.fileInfoSize);
        cursor = prefix;
        for (s = 0; s < sc; s++) {
            TSector src = gr2.sectors[s];
            size_t fix_bytes = (size_t)fl[s].n * sizeof(TFixUpData);
            size_t mar_bytes = (size_t)src.marshallSize * sizeof(TMarshallData);
            if (fl[s].n && fl[s].fd) {
                memcpy(buf + cursor, fl[s].fd, fix_bytes);
                outsecs[s].fixupOffset = (uint32_t)cursor;
                outsecs[s].fixupSize = fl[s].n;
                cursor += fix_bytes;
            }
            if (src.marshallSize) {
                memcpy(buf + cursor, file + src.marshallOffset, mar_bytes);
                outsecs[s].marshallOffset = (uint32_t)cursor;
                cursor += mar_bytes;
            }
            if (seclen[s] && secs[s])
                memcpy(buf + cursor, secs[s], seclen[s]);
            outsecs[s].dataOffset = (uint32_t)cursor;
            outsecs[s].compressType = COMPRESSION_TYPE_NONE;
            outsecs[s].compressedLen = seclen[s];
            outsecs[s].decompressLen = seclen[s];
            cursor += seclen[s];
        }
        info = (TFileInfo *)(buf + sizeof(THeader));
        info->totalSize = (uint32_t)total;
        info->crc32 = CRC32(buf + sizeof(THeader) + info->fileInfoSize, total - sizeof(THeader) - info->fileInfoSize);
        out = fopen(out_path, "wb");
        if (!out || fwrite(buf, 1, total, out) != total) {
            if (out)
                fclose(out);
            free(buf);
            snprintf(log, log_len, "Cannot write %s", out_path);
            goto done;
        }
        fclose(out);
        free(buf);
    }
    snprintf(log, log_len, "Wrote %s  (%d vertices, %d triangles, %d bones, %s template)", out_path, total_v, total_i / 3, wrote_bones, skinned ? "skinned" : "rigid");
    ok = 1;
done:
    if (secs) {
        uint32_t s;
        for (s = 0; s < gr2.fileInfo.sectorCount; s++)
            free(secs[s]);
        free(secs);
    }
    free(seclen);
    free(newv);
    free(newi);
    free(pattern);
    if (fl) {
        uint32_t s;
        for (s = 0; s < gr2.fileInfo.sectorCount; s++)
            free(fl[s].fd);
        free(fl);
    }
    Gr2_Free(&gr2);
    free(file);
    cn6_free(&cn6);
    return ok;
}

static void usage(void)
{
    fprintf(stderr,
            "CivNexus6 Linux\n"
            "  civnexus6 info <file.fgx>\n"
            "  civnexus6 export-cn6 <file.fgx> <file.cn6>\n"
            "  civnexus6 import-cn6 <template.fgx> <model.cn6> <output.fgx>\n"
            "  civnexus6 create-cn6 <model.cn6> <output.fgx>\n"
            "  civnexus6 export-na2 <file.fgx> <file.na2>\n"
            "  civnexus6 [file.fgx]          open the window\n");
}

int main(int argc, char **argv)
{
    if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        usage();
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "info")) {
        TGr2 g;
        uint8_t *b = NULL;
        char text[16000];
        if (!load_fgx(argv[2], &g, &b)) {
            fprintf(stderr, "Cannot read %s\n", argv[2]);
            return 1;
        }
        info_text(&g, argv[2], text, sizeof text);
        fputs(text, stdout);
        Gr2_Free(&g);
        free(b);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "export-cn6")) {
        TGr2 g;
        uint8_t *b = NULL;
        char log[256];
        int ok;
        if (!load_fgx(argv[2], &g, &b)) {
            fprintf(stderr, "Cannot read %s\n", argv[2]);
            return 1;
        }
        ok = export_cn6(&g, argv[3], log, sizeof log);
        fprintf(ok ? stdout : stderr, "%s\n", log);
        Gr2_Free(&g);
        free(b);
        return ok ? 0 : 1;
    }
    if (argc >= 5 && !strcmp(argv[1], "import-cn6")) {
        TGr2 g;
        uint8_t *b = NULL;
        char log[512];
        int ok;
        FILE *fp;
        long n;
        if (!load_fgx(argv[2], &g, &b)) {
            fprintf(stderr, "Cannot read %s\n", argv[2]);
            return 1;
        }
        fp = fopen(argv[2], "rb");
        if (!fp) {
            fprintf(stderr, "Cannot read %s\n", argv[2]);
            Gr2_Free(&g);
            free(b);
            return 1;
        }
        fseek(fp, 0, SEEK_END);
        n = ftell(fp);
        fclose(fp);
        ok = import_cn6(&g, b, (size_t)n, argv[3], argv[4], log, sizeof log);
        fprintf(ok ? stdout : stderr, "%s\n", log);
        Gr2_Free(&g);
        free(b);
        return ok ? 0 : 1;
    }
    if (argc >= 4 && !strcmp(argv[1], "create-cn6")) {
        char log[512];
        int ok = create_cn6(argv[2], argv[3], log, sizeof log);
        fprintf(ok ? stdout : stderr, "%s\n", log);
        return ok ? 0 : 1;
    }
    if (argc >= 4 && !strcmp(argv[1], "export-na2")) {
        TGr2 g;
        uint8_t *b = NULL;
        char log[256];
        int ok;
        if (!load_fgx(argv[2], &g, &b)) {
            fprintf(stderr, "Cannot read %s\n", argv[2]);
            return 1;
        }
        ok = export_na2(&g, argv[3], log, sizeof log);
        fprintf(ok ? stdout : stderr, "%s\n", log);
        Gr2_Free(&g);
        free(b);
        return ok ? 0 : 1;
    }
#ifdef CIVNEXUS_GUI
    return run_gui(argc >= 2 ? argv[1] : NULL);
#else
    if (argc >= 2) {
        TGr2 g;
        uint8_t *b = NULL;
        char text[16000], out[1100], log[256];
        const char *dot;
        if (!load_fgx(argv[1], &g, &b)) {
            fprintf(stderr, "Cannot read %s\n", argv[1]);
            usage();
            return 1;
        }
        info_text(&g, argv[1], text, sizeof text);
        fputs(text, stdout);
        snprintf(out, sizeof out, "%s", argv[1]);
        dot = strrchr(out, '.');
        if (dot && (!strcmp(dot, ".fgx") || !strcmp(dot, ".FGX") || !strcmp(dot, ".gr2")))
            snprintf((char *)dot, sizeof out - (size_t)(dot - out), ".cn6");
        else
            strncat(out, ".cn6", sizeof out - strlen(out) - 1);
        if (!export_cn6(&g, out, log, sizeof log))
            fprintf(stderr, "%s\n", log);
        else
            fprintf(stdout, "\n%s\n", log);
        Gr2_Free(&g);
        free(b);
        return 0;
    }
    usage();
    return 0;
#endif
}
