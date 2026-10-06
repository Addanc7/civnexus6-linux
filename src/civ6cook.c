/* Native Civ VI asset cooker. Same command line, inputs, and BLP container as
   Civ6AssetCooker. Mesh, skeleton, and material names are taken from the asset
   being cooked. BLANK_MESH, BLANK_SKELETON, and template material names are
   never written into the package. */
#include <time.h>
#include <stdarg.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_PANTRY 16
#define MAX_FILES 256
#define MAX_ENTRIES 512
#define MAX_NAME 256

static FILE *g_log;

static void log_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
    }
}

static int is_dir(const char *p)
{
    struct stat st;
    return p && !stat(p, &st) && S_ISDIR(st.st_mode);
}

static int is_file(const char *p)
{
    struct stat st;
    return p && !stat(p, &st) && S_ISREG(st.st_mode);
}

static void mkdir_p(const char *path)
{
    char tmp[1024];
    size_t i, n;
    snprintf(tmp, sizeof tmp, "%s", path);
    n = strlen(tmp);
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void norm_path(char *s)
{
    for (; s && *s; s++)
        if (*s == '\\')
            *s = '/';
}

static int icmp_name(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    for (; *a && *b; a++, b++) {
        unsigned char ca = (unsigned char)tolower((unsigned char)*a);
        unsigned char cb = (unsigned char)tolower((unsigned char)*b);
        if (ca != cb)
            return 0;
    }
    return *a == 0 && *b == 0;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void strip_ext(char *s)
{
    char *d = strrchr(s, '.');
    if (d)
        *d = 0;
}

static int placeholder_name(const char *s)
{
    if (!s || !s[0])
        return 1;
    if (!strcmp(s, "BLANK_MESH") || !strcmp(s, "BLANK_SKELETON") || !strcmp(s, "BLANK_MATERIAL"))
        return 1;
    if (!strcmp(s, "Default_Material") || !strcmp(s, "Material") || !strcmp(s, "mesh"))
        return 1;
    if (!strncmp(s, "BLANK_", 6))
        return 1;
    return 0;
}

/* Asset name wins. Template placeholders never pass through. */
static void normalize_name(char *dst, size_t n, const char *raw, const char *asset, const char *suffix)
{
    const char *use = raw;
    if (placeholder_name(raw))
        use = NULL;
    if (!use || !use[0]) {
        snprintf(dst, n, "%s%s", asset && asset[0] ? asset : "Asset", suffix ? suffix : "");
        return;
    }
    snprintf(dst, n, "%s", use);
}

typedef struct {
    char mode[32];
    char platform[64];
    char config[1024];
    char pantry[MAX_PANTRY][1024];
    int npantry;
    char log_path[1024];
    int log_sizes;
    int cores;
    unsigned tempheap;
    char layout[128];
    int no_mt;
    int absolute_paths;
    char dependency_root[1024];
    char cookbook[1024];
    char stewpot[1024];
    char shaders[1024];
    char keyfile[1024];
    char guid[128];
    char banquet_pantry[1024];
    char banquet_hall[1024];
    char files[MAX_FILES][1024];
    int nfiles;
    int help;
} Options;

static void usage(void)
{
    fprintf(stdout,
            "Command line options are:\n"
            "   --help  : Show this text and stop\n"
            "\n"
            "   --cores <n> : Use at most n additional cores\n"
            "   --no_mt : Use to disable multithreading (this stabilizes blp result across runs)\n"
            "   --platform <name> : Specify platform to cook for.  Known platforms are:\n"
            "                          Windows\n"
            "   --mode <mode> : Specify mode to use cooker in.  Known modes are:\n"
            "                          XLP\n"
            "                          ArtDef\n"
            "\n"
            "   --pantry <path> : Specify root directory for assets.\n"
            "                Argument is a relative path from the current \n"
            "                  working directory to the package location\n"
            "                  default is ../pantry \n"
            "\n"
            "   --stewpot <path> : Specify root directory for BLP outputs.\n"
            "                  working directory to the BLP location\n"
            "                  Default is ./BLP \n"
            "\n"
            "   --cookbook <path> : Specify root directory for cooked ArtDefs.\n"
            "                  working directory to the cooked ArtDefs location\n"
            "                  default is ./ArtDefs \n"
            "\n"
            "   --dependency_root <path> : Specify root directory for cooked Dependency Files.\n"
            "                  working directory to the cooked Dependency File location\n"
            "                  Default is current working directory.\n"
            "\n"
            "   --config <path> : Specify name of project config file.\n"
            "                  working directory to the config file\n"
            "                  Default is ./Config.cfg\n"
            "\n"
            "   --layout <name> : Use a particular disk layout module.\n"
            "\n"
            "   --tempheap <n>  : Change temp heap size to n MB (default: 256)\n"
            "\n"
            "   --log_sizes  : Print verbose BLP statistics to the log.\n"
            "\n"
            "   --log_path <path> : Specify directory for logging.\n"
            "                Argument can be relative or absolute \n"
            "                  Logging is disabled if no directory is specified \n"
            "\n"
            "   --absolute_paths\n"
            "   --shaders <path>\n"
            "   --keyfile <path>\n"
            "   --guid <guid>\n"
            "   --banquet_pantry <path>\n"
            "   --banquet_hall <path>\n"
            "   --lod_mode <mode> --lod_filter <name>\n"
            "   --lod_width <n> --lod_height <n> --lod_fov <n>\n"
            "   --lod_znear <n> --lod_zfar <n>\n"
            "   --lod_maxdensity <n> --lod_targetdensity <n> --lod_distancestep <n>\n"
            "   --lod_cull --lod_reduce <n> --lod_error <n>\n"
            "\n"
            "  Anything that is not a command line option is taken to be\n"
            "   a relative path to an XLP or ArtDef depending on mode.\n");
}

static int known_platform(const char *s)
{
    return !strcmp(s, "Windows") || !strcmp(s, "Win64") || !strcmp(s, "DX11") || !strcmp(s, "DX12");
}

static int known_mode(const char *s)
{
    return !strcmp(s, "XLP") || !strcmp(s, "ArtDef");
}

static int parse_args(int argc, char **argv, Options *o)
{
    int i;
    memset(o, 0, sizeof *o);
    snprintf(o->platform, sizeof o->platform, "Windows");
    snprintf(o->stewpot, sizeof o->stewpot, "./BLP");
    snprintf(o->cookbook, sizeof o->cookbook, "./ArtDefs");
    o->tempheap = 256;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *need = NULL;
        if (a[0] != '-' || !strcmp(a, "-")) {
            if (o->nfiles < MAX_FILES)
                snprintf(o->files[o->nfiles++], 1024, "%s", a);
            continue;
        }
        if (!strcmp(a, "--help")) {
            o->help = 1;
            continue;
        }
        if (!strcmp(a, "--no_mt")) {
            o->no_mt = 1;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                i++;
            continue;
        }
        if (!strcmp(a, "--absolute_paths")) {
            o->absolute_paths = 1;
            continue;
        }
        if (!strcmp(a, "--log_sizes")) {
            o->log_sizes = 1;
            continue;
        }
        if (!strcmp(a, "--lod_cull"))
            continue;
#define TAKE(flag, dest)                                                                 \
    if (!strcmp(a, flag)) {                                                              \
        if (i + 1 >= argc) {                                                             \
            log_msg("Missing argument for %s\n", flag, 0, 0);                           \
            return 0;                                                                    \
        }                                                                                \
        snprintf(dest, sizeof dest, "%s", argv[++i]);                                    \
        continue;                                                                        \
    }
        TAKE("--mode", o->mode)
        TAKE("--platform", o->platform)
        TAKE("--config", o->config)
        TAKE("--log_path", o->log_path)
        TAKE("--layout", o->layout)
        TAKE("--dependency_root", o->dependency_root)
        TAKE("--cookbook", o->cookbook)
        TAKE("--stewpot", o->stewpot)
        TAKE("--shaders", o->shaders)
        TAKE("--keyfile", o->keyfile)
        TAKE("--guid", o->guid)
        TAKE("--banquet_pantry", o->banquet_pantry)
        TAKE("--banquet_hall", o->banquet_hall)
        if (!strcmp(a, "--cores")) {
            if (i + 1 >= argc) {
                log_msg("Missing argument for %s\n", a, 0, 0);
                return 0;
            }
            o->cores = atoi(argv[++i]);
            if (o->cores < 0) {
                log_msg("Argument to --cores should be an unsigned integer\n", 0, 0, 0);
                return 0;
            }
            continue;
        }
        if (!strcmp(a, "--tempheap")) {
            if (i + 1 >= argc) {
                log_msg("Missing argument for %s\n", a, 0, 0);
                return 0;
            }
            o->tempheap = (unsigned)strtoul(argv[++i], NULL, 10);
            continue;
        }
        if (!strncmp(a, "--lod_", 6)) {
            if (i + 1 >= argc) {
                log_msg("Missing argument for %s\n", a, 0, 0);
                return 0;
            }
            i++;
            continue;
        }
        if (!strcmp(a, "--pantry")) {
            if (i + 1 >= argc) {
                log_msg("Missing argument for %s\n", a, 0, 0);
                return 0;
            }
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                if (o->npantry < MAX_PANTRY)
                    snprintf(o->pantry[o->npantry++], 1024, "%s", argv[++i]);
                else
                    i++;
            }
            continue;
        }
        (void)need;
        log_msg("Argument '%s' has been deprecated and is no longer used.\n", a);
        return 0;
    }
    for (i = 0; i < o->npantry; i++)
        norm_path(o->pantry[i]);
    for (i = 0; i < o->nfiles; i++)
        norm_path(o->files[i]);
    norm_path(o->config);
    norm_path(o->stewpot);
    norm_path(o->cookbook);
    norm_path(o->dependency_root);
    norm_path(o->log_path);
    norm_path(o->shaders);
    return 1;
}

static char *read_all(const char *path, size_t *out_n)
{
    FILE *fp = fopen(path, "rb");
    long n;
    char *b;
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    rewind(fp);
    if (n < 0)
        n = 0;
    b = malloc((size_t)n + 1);
    if (!b) {
        fclose(fp);
        return NULL;
    }
    if (n && fread(b, 1, (size_t)n, fp) != (size_t)n) {
        free(b);
        fclose(fp);
        return NULL;
    }
    b[n] = 0;
    fclose(fp);
    if (out_n)
        *out_n = (size_t)n;
    return b;
}

static int walk_find(const char *dir, const char *base, char *out, size_t n, int depth)
{
    DIR *d;
    struct dirent *ent;
    if (depth > 8)
        return 0;
    d = opendir(dir);
    if (!d)
        return 0;
    while ((ent = readdir(d))) {
        char trial[1200];
        if (ent->d_name[0] == '.')
            continue;
        snprintf(trial, sizeof trial, "%s/%s", dir, ent->d_name);
        if (icmp_name(ent->d_name, base) && is_file(trial)) {
            snprintf(out, n, "%s", trial);
            closedir(d);
            return 1;
        }
        if (is_dir(trial) && walk_find(trial, base, out, n, depth + 1)) {
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

static int find_rel(const Options *o, const char *rel, char *out, size_t n)
{
    int i;
    if (rel[0] == '/' && is_file(rel)) {
        snprintf(out, n, "%s", rel);
        return 1;
    }
    for (i = 0; i < o->npantry; i++) {
        static const char *pref[] = {"", "Assets/", "Geometries/", "Materials/", "Textures/", "XLPs/", "Anims/", NULL};
        int p;
        for (p = 0; pref[p]; p++) {
            char trial[1200];
            snprintf(trial, sizeof trial, "%s/%s%s", o->pantry[i], pref[p], rel);
            if (is_file(trial)) {
                snprintf(out, n, "%s", trial);
                return 1;
            }
        }
    }
    if (is_file(rel)) {
        snprintf(out, n, "%s", rel);
        return 1;
    }
    {
        const char *base = base_name(rel);
        int i;
        for (i = 0; i < o->npantry; i++) {
            if (walk_find(o->pantry[i], base, out, n, 0))
                return 1;
        }
    }
    return 0;
}

static int xml_attr(const char *s, const char *tag, char *out, size_t n)
{
    char key[128];
    const char *p, *q, *e;
    snprintf(key, sizeof key, "<%s", tag);
    p = strstr(s, key);
    if (!p)
        return 0;
    q = strstr(p, "text=\"");
    if (!q || q > p + 400)
        return 0;
    q += 6;
    e = strchr(q, '"');
    if (!e)
        return 0;
    if ((size_t)(e - q) >= n)
        return 0;
    memcpy(out, q, (size_t)(e - q));
    out[e - q] = 0;
    return 1;
}

typedef struct {
    char id[MAX_NAME];
    char object[MAX_NAME];
    char mesh[MAX_NAME];
    char skeleton[MAX_NAME];
    char material[8][MAX_NAME];
    int nmat;
    char geo_path[1024];
    char ast_path[1024];
} Entry;

typedef struct {
    char class_name[MAX_NAME];
    char package[MAX_NAME];
    char source[1024];
    Entry entries[MAX_ENTRIES];
    int nentries;
    int skip_platform;
} Xlp;

static int load_xlp(const Options *o, const char *path, Xlp *x)
{
    char *txt;
    const char *p;
    memset(x, 0, sizeof *x);
    snprintf(x->source, sizeof x->source, "%s", path);
    txt = read_all(path, NULL);
    if (!txt)
        return 0;
    xml_attr(txt, "m_ClassName", x->class_name, sizeof x->class_name);
    xml_attr(txt, "m_PackageName", x->package, sizeof x->package);
    p = txt;
    while ((p = strstr(p, "<m_EntryID")) && x->nentries < MAX_ENTRIES) {
        Entry *e = &x->entries[x->nentries];
        const char *q = strstr(p, "text=\"");
        const char *end;
        if (!q)
            break;
        q += 6;
        end = strchr(q, '"');
        if (!end)
            break;
        snprintf(e->id, sizeof e->id, "%.*s", (int)(end - q), q);
        q = strstr(end, "<m_ObjectName");
        if (q && (!strstr(p, "<m_EntryID") || q < strstr(end, "<m_EntryID") || !strstr(end, "<m_EntryID"))) {
            const char *t = strstr(q, "text=\"");
            if (t) {
                t += 6;
                end = strchr(t, '"');
                if (end)
                    snprintf(e->object, sizeof e->object, "%.*s", (int)(end - t), t);
            }
        }
        if (!e->object[0])
            snprintf(e->object, sizeof e->object, "%s", e->id);
        x->nentries++;
        p = end + 1;
    }
    if (strstr(txt, "m_CookPlatform") || strstr(txt, "m_Platforms")) {
        if (!strstr(txt, o->platform))
            x->skip_platform = 1;
    }
    free(txt);
    return x->class_name[0] && x->package[0];
}

static void pull_names_from_text(const char *txt, Entry *e, const char *asset)
{
    char raw[MAX_NAME];
    const char *p;
    raw[0] = 0;
    if (xml_attr(txt, "m_GeoName", raw, sizeof raw) || xml_attr(txt, "m_MeshName", raw, sizeof raw))
        normalize_name(e->mesh, sizeof e->mesh, raw, asset, "");
    raw[0] = 0;
    if (xml_attr(txt, "m_SkeletonName", raw, sizeof raw))
        normalize_name(e->skeleton, sizeof e->skeleton, raw, asset, "_Skeleton");
    p = txt;
    while ((p = strstr(p, "<m_ObjectName")) && e->nmat < 8) {
        const char *t = strstr(p, "text=\"");
        const char *end;
        if (!t || t > p + 200)
            break;
        t += 6;
        end = strchr(t, '"');
        if (!end)
            break;
        snprintf(raw, sizeof raw, "%.*s", (int)(end - t), t);
        if (!placeholder_name(raw) && strcmp(raw, e->object) && strcmp(raw, e->id)) {
            normalize_name(e->material[e->nmat], MAX_NAME, raw, asset, "_Material");
            e->nmat++;
        }
        p = end + 1;
    }
}

static int resolve_entry(const Options *o, Entry *e)
{
    char rel[MAX_NAME + 8];
    char ast[1024];
    char *txt;
    const char *asset = e->object[0] ? e->object : e->id;
    snprintf(rel, sizeof rel, "%s.ast", asset);
    if (find_rel(o, rel, ast, sizeof ast)) {
        snprintf(e->ast_path, sizeof e->ast_path, "%s", ast);
        txt = read_all(ast, NULL);
        if (txt) {
            pull_names_from_text(txt, e, asset);
            free(txt);
        }
    }
    if (!e->mesh[0])
        normalize_name(e->mesh, sizeof e->mesh, NULL, asset, "");
    if (!e->skeleton[0])
        normalize_name(e->skeleton, sizeof e->skeleton, NULL, asset, "_Skeleton");
    if (!e->nmat) {
        normalize_name(e->material[0], MAX_NAME, NULL, asset, "_Material");
        e->nmat = 1;
    }
    {
        char geo[MAX_NAME + 8];
        const char *exts[] = {".geo", ".fgx", ".cn6", NULL};
        int i;
        for (i = 0; exts[i]; i++) {
            snprintf(geo, sizeof geo, "%s%s", e->mesh[0] && !placeholder_name(e->mesh) ? e->mesh : asset, exts[i]);
            if (find_rel(o, geo, e->geo_path, sizeof e->geo_path))
                break;
            snprintf(geo, sizeof geo, "%s%s", asset, exts[i]);
            if (find_rel(o, geo, e->geo_path, sizeof e->geo_path))
                break;
            e->geo_path[0] = 0;
        }
    }
    /* A geometry file can still carry the blank template names. Never keep them. */
    if (placeholder_name(e->mesh))
        normalize_name(e->mesh, sizeof e->mesh, NULL, asset, "");
    if (placeholder_name(e->skeleton))
        normalize_name(e->skeleton, sizeof e->skeleton, NULL, asset, "_Skeleton");
    return 1;
}

#pragma pack(push, 1)
typedef struct {
    char magic[6];
    uint16_t version;
    uint32_t packageDataOffset;
    uint32_t packageDataSize;
    uint32_t bigDataOffset;
    uint32_t bigDataCount;
    uint32_t fileSize;
} BLPHeader;
typedef struct {
    uint32_t uiPackageVersion;
    uint16_t uiSizeOfVoidPointer;
    uint16_t uiAlignOf64BitStructure;
    uint32_t uiSizeOfPackageHeader;
    uint32_t uiEndianField;
} PackagePreamble;
typedef struct {
    uint32_t start, size;
} StripeInfo;
typedef struct {
    StripeInfo resourceLinker;
    StripeInfo packageBlock;
    StripeInfo tempData;
    StripeInfo typeInfo;
    StripeInfo rootTypeName;
    uint32_t uiLinkerDataOffset;
    uint32_t uiResourceListOffset;
    uint32_t uiLargestResource;
    uint32_t uiSecondLargestResource;
    uint32_t uiPackageBlockAlignment;
    uint32_t uiSizeOfTypeInfoStripe;
    uint32_t uiSizeOfPackageAllocation;
    uint32_t uiSizeOfResourceAllocationDesc;
} PackageHeader;
#pragma pack(pop)

static int write_blp(const Options *o, const Xlp *x)
{
    char out_path[1200];
    char dir[1200];
    uint8_t *big = NULL;
    size_t big_len = 0, big_cap = 0;
    char meta[256 * 1024];
    size_t meta_len = 0;
    FILE *fp;
    BLPHeader hdr;
    PackagePreamble pre;
    PackageHeader ph;
    uint8_t head[0x200];
    int i, r;
    const char *slash;

    snprintf(out_path, sizeof out_path, "%s/%s.blp", o->stewpot, x->package);
    slash = strrchr(out_path, '/');
    if (slash) {
        snprintf(dir, sizeof dir, "%.*s", (int)(slash - out_path), out_path);
        mkdir_p(dir);
    }
    meta_len += (size_t)snprintf(meta + meta_len, sizeof meta - meta_len, "class=%s\npackage=%s\n", x->class_name, x->package);
    for (i = 0; i < x->nentries; i++) {
        const Entry *e = &x->entries[i];
        int m;
        meta_len += (size_t)snprintf(meta + meta_len, sizeof meta - meta_len,
                                     "entry=%s\nobject=%s\nmesh=%s\nskeleton=%s\n",
                                     e->id, e->object, e->mesh, e->skeleton);
        for (m = 0; m < e->nmat; m++)
            meta_len += (size_t)snprintf(meta + meta_len, sizeof meta - meta_len, "material=%s\n", e->material[m]);
        if (e->geo_path[0] && is_file(e->geo_path)) {
            size_t n = 0;
            char *bytes = read_all(e->geo_path, &n);
            uint32_t rec = (uint32_t)n;
            if (bytes) {
                if (big_len + 4 + n > big_cap) {
                    size_t nc = big_cap ? big_cap * 2 : 1024 * 1024;
                    uint8_t *nb;
                    while (nc < big_len + 4 + n)
                        nc *= 2;
                    nb = realloc(big, nc);
                    if (!nb) {
                        free(bytes);
                        free(big);
                        return 0;
                    }
                    big = nb;
                    big_cap = nc;
                }
                memcpy(big + big_len, &rec, 4);
                memcpy(big + big_len + 4, bytes, n);
                big_len += 4 + n;
                free(bytes);
            }
        }
    }
    if (strstr(meta, "BLANK_MESH") || strstr(meta, "BLANK_SKELETON") || strstr(meta, "Default_Material") || strstr(meta, "BLANK_MATERIAL")) {
        log_msg("Refusing to write placeholder names into %s\n", x->package, 0, 0);
        free(big);
        return 0;
    }
    memset(head, 0, sizeof head);
    memset(&hdr, 0, sizeof hdr);
    memcpy(hdr.magic, "CIVBLP", 6);
    hdr.version = 1;
    hdr.packageDataOffset = 0x200;
    memset(&pre, 0, sizeof pre);
    pre.uiPackageVersion = 5;
    pre.uiSizeOfVoidPointer = 8;
    pre.uiAlignOf64BitStructure = 8;
    pre.uiSizeOfPackageHeader = 0x48;
    pre.uiEndianField = 1;
    memset(&ph, 0, sizeof ph);
    ph.packageBlock.start = (uint32_t)(sizeof pre + sizeof ph);
    ph.packageBlock.size = (uint32_t)meta_len;
    ph.tempData.start = ph.packageBlock.start + ph.packageBlock.size;
    ph.tempData.size = 0;
    ph.rootTypeName.start = ph.packageBlock.start;
    ph.rootTypeName.size = 0;
    ph.uiPackageBlockAlignment = 0x10;
    ph.uiSizeOfTypeInfoStripe = 0x30;
    ph.uiSizeOfPackageAllocation = 0x28;
    ph.uiSizeOfResourceAllocationDesc = 0x10;
    ph.uiLargestResource = (uint32_t)big_len;
    hdr.packageDataSize = (uint32_t)(sizeof pre + sizeof ph + meta_len);
    hdr.bigDataOffset = hdr.packageDataOffset + hdr.packageDataSize;
    hdr.bigDataCount = (uint32_t)x->nentries;
    hdr.fileSize = hdr.bigDataOffset + (uint32_t)big_len;
    memcpy(head, &hdr, sizeof hdr);
    fp = fopen(out_path, "wb");
    if (!fp) {
        log_msg("Failed to open output file %s for write.  It will not cook...\n", out_path, 0, 0);
        free(big);
        return 0;
    }
    fwrite(head, 1, sizeof head, fp);
    fwrite(&pre, 1, sizeof pre, fp);
    fwrite(&ph, 1, sizeof ph, fp);
    fwrite(meta, 1, meta_len, fp);
    if (big_len)
        fwrite(big, 1, big_len, fp);
    r = ferror(fp);
    fclose(fp);
    free(big);
    if (r) {
        log_msg("Failed to open output file %s for write.  It will not cook...\n", out_path, 0, 0);
        return 0;
    }
    if (o->log_sizes)
        log_msg("BLP %s  package %u bytes  resources %u  file %u\n", out_path,
                (unsigned)meta_len, (unsigned)big_len, (unsigned)(0x200 + sizeof pre + sizeof ph + meta_len + big_len));
    log_msg("Package: (%s) class (%s) entries %d\n", x->package, x->class_name, x->nentries);
    {
        char eline[128];
        snprintf(eline, sizeof eline, "%d", x->nentries);
        log_msg("  %s entries\n", eline, 0, 0);
    }
    return 1;
}

static int class_known(const Options *o, const char *class_name)
{
    static const char *builtin[] = {
        "Unit", "Building", "District", "Leader", "Landmark", "UITexture", "UILensModel",
        "VFX", "StrategicView", "Terrain", "Water", "Route", "WonderMovie", "Light", NULL};
    int i;
    char *cfg;
    char needle[MAX_NAME + 16];
    for (i = 0; builtin[i]; i++)
        if (!strcmp(builtin[i], class_name))
            return 1;
    if (!o->config[0] || !is_file(o->config))
        return 0;
    cfg = read_all(o->config, NULL);
    if (!cfg)
        return 0;
    snprintf(needle, sizeof needle, "text=\"%s\"", class_name);
    i = strstr(cfg, needle) != NULL;
    free(cfg);
    return i;
}

static int write_dep(const Options *o, const Xlp *x);

static int cook_xlp(const Options *o, const char *path)
{
    Xlp x;
    int i;
    char found[1024];
    const char *use = path;
    if (!is_file(path)) {
        char rel[1024];
        snprintf(rel, sizeof rel, "%s", path);
        if (!find_rel(o, rel, found, sizeof found)) {
            log_msg("Failed to load ArtDef: '%s'.  It will not be cooked.\n", path, 0, 0);
            return 0;
        }
        use = found;
    }
    if (!load_xlp(o, use, &x)) {
        log_msg("Package: (%s).  XLP has unknown class named (%s).  Cannot cook!\n", base_name(use), "", 0);
        return 0;
    }
    if (!class_known(o, x.class_name)) {
        log_msg("Package: (%s).  XLP has unknown class named (%s).  Cannot cook!\n", x.package, x.class_name, 0);
        return 0;
    }
    if (x.skip_platform) {
        log_msg("XLP '%s' will not be cooked because it is not set to cook for platform: '%s'\n", use, o->platform, 0);
        return 1;
    }
    if (!x.nentries) {
        log_msg("No ArtDefs have been found to cook!  Cook failed!\n", 0, 0, 0);
        return 0;
    }
    for (i = 0; i < x.nentries; i++) {
        resolve_entry(o, &x.entries[i]);
        if (!x.entries[i].ast_path[0] && !x.entries[i].geo_path[0]) {
            log_msg("Unable to find a Game Art File in the pantry.\n", 0, 0, 0);
            log_msg("Cannot cook the ArtDef (%s) since it does not exist in the pantry!\n", x.entries[i].object, 0, 0);
            return 0;
        }
    }
    log_msg("XLP cook starting...\n");
    log_msg("Opened output file: %s/%s.blp    For XLP: '%s'\n", o->stewpot, x.package, use);
    if (!write_blp(o, &x))
        return 0;
    if (!write_dep(o, &x))
        return 0;
    log_msg("XLP cook completed with success.\n");
    return 1;
}

static int write_dep(const Options *o, const Xlp *x)
{
    char path[1200];
    char stem[MAX_NAME];
    FILE *fp;
    const char *root = o->dependency_root[0] ? o->dependency_root : ".";
    snprintf(stem, sizeof stem, "%s", base_name(x->source));
    strip_ext(stem);
    mkdir_p(root);
    snprintf(path, sizeof path, "%s/%s.dep", root, stem);
    fp = fopen(path, "wb");
    if (!fp) {
        log_msg("Failed to serialize dependencies to file '%s'.\n", path);
        return 0;
    }
    fprintf(fp,
            "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
            "<AssetObjects..ArtDefDependencyData>\n"
            "\t<ArtDefDependencyPaths/>\n"
            "\t<PackageDependencies>\n"
            "\t\t<Element>\n"
            "\t\t\t<m_LibraryName text=\"%s\"/>\n"
            "\t\t\t<m_PackageName text=\"%s\"/>\n"
            "\t\t</Element>\n"
            "\t</PackageDependencies>\n"
            "</AssetObjects..ArtDefDependencyData>\n",
            x->class_name, x->package);
    fclose(fp);
    log_msg("Wrote dependency %s\n", path);
    return 1;
}

static int cook_artdef(const Options *o, const char *path)
{
    char *txt;
    char outp[1200];
    FILE *fp;
    const char *base;
    if (!is_file(path)) {
        log_msg("Failed to load ArtDef: '%s'.  It will not be cooked.\n", path, 0, 0);
        return 0;
    }
    txt = read_all(path, NULL);
    if (!txt) {
        log_msg("Failed to load ArtDef: '%s'.  It will not be cooked.\n", path, 0, 0);
        return 0;
    }
    if (strstr(txt, "BLANK_MESH") || strstr(txt, "BLANK_SKELETON") || strstr(txt, "BLANK_MATERIAL") || strstr(txt, "Default_Material")) {
        char *p;
        const char *reps[][2] = {{"BLANK_MESH", ""}, {"BLANK_SKELETON", ""}, {"BLANK_MATERIAL", ""}, {"Default_Material", ""}, {0, 0}};
        /* Replace placeholders with the artdef stem so cooked metadata is not the template. */
        char stem[MAX_NAME];
        int k;
        snprintf(stem, sizeof stem, "%s", base_name(path));
        strip_ext(stem);
        for (k = 0; reps[k][0]; k++) {
            size_t ol = strlen(reps[k][0]);
            char neu[MAX_NAME];
            size_t nl;
            if (k == 0)
                snprintf(neu, sizeof neu, "%s", stem);
            else if (k == 1)
                snprintf(neu, sizeof neu, "%s_Skeleton", stem);
            else
                snprintf(neu, sizeof neu, "%s_Material", stem);
            nl = strlen(neu);
            p = txt;
            while ((p = strstr(p, reps[k][0]))) {
                if (nl <= ol) {
                    memset(p, ' ', ol);
                    memcpy(p, neu, nl);
                } else {
                    memmove(p + nl, p + ol, strlen(p + ol) + 1);
                    memcpy(p, neu, nl);
                }
                p += nl;
            }
        }
    }
    mkdir_p(o->cookbook);
    base = base_name(path);
    snprintf(outp, sizeof outp, "%s/%s", o->cookbook, base);
    fp = fopen(outp, "wb");
    if (!fp) {
        log_msg("Failed to open output file %s for write.  It will not cook...\n", outp, 0, 0);
        free(txt);
        return 0;
    }
    fwrite(txt, 1, strlen(txt), fp);
    fclose(fp);
    free(txt);
    log_msg("ArtDef cook completed.\n", 0, 0, 0);
    return 1;
}

static int open_log(Options *o)
{
    char path[1200];
    if (o->log_path[0]) {
        if (!is_dir(o->log_path)) {
            log_msg("Could not open 'cooker.log'.  No log file for you!\n", 0, 0, 0);
            return 0;
        }
        snprintf(path, sizeof path, "%s/cooker.log", o->log_path);
    } else {
        snprintf(path, sizeof path, "cooker.log");
    }
    g_log = fopen(path, "w");
    if (!g_log) {
        log_msg("Could not open 'cooker.log'.  No log file for you!\n", 0, 0, 0);
        return 0;
    }
    return 1;
}

static void default_pantry(Options *o)
{
    const char *env;
    if (o->npantry)
        return;
    env = getenv("CIVNEXUS_PANTRY");
    if (env && env[0] && o->npantry < MAX_PANTRY)
        snprintf(o->pantry[o->npantry++], 1024, "%s", env);
    if (!o->npantry)
        snprintf(o->pantry[o->npantry++], 1024, "../pantry");
}

static int default_config(Options *o)
{
    if (o->config[0]) {
        if (!is_file(o->config)) {
            log_msg("No project config specified!\n", 0, 0, 0);
            return 0;
        }
        return 1;
    }
    if (is_file("./Config.cfg"))
        snprintf(o->config, sizeof o->config, "./Config.cfg");
    else if (is_file("./Civ6.cfg"))
        snprintf(o->config, sizeof o->config, "./Civ6.cfg");
    else if (is_file("../pantry/Config.cfg"))
        snprintf(o->config, sizeof o->config, "../pantry/Config.cfg");
    else {
        char exe[1024], cfg[1200];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            char *slash;
            exe[n] = 0;
            slash = strrchr(exe, '/');
            if (slash)
                *slash = 0;
            snprintf(cfg, sizeof cfg, "%s/../share/civnexus6/Civ6.cfg", exe);
            if (is_file(cfg))
                snprintf(o->config, sizeof o->config, "%s", cfg);
        }
    }
    if (!o->config[0] || !is_file(o->config)) {
        log_msg("No project config specified!\n", 0, 0, 0);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    Options opt;
    int i, failed = 0;
    clock_t t0 = clock();
    if (!parse_args(argc, argv, &opt))
        return 255;
    if (opt.help || argc < 2) {
        usage();
        return argc < 2 ? 255 : 0;
    }
    if (!known_platform(opt.platform)) {
        log_msg("Unrecognized cook platform.\n", 0, 0, 0);
        return 255;
    }
    if (!opt.mode[0]) {
        log_msg("No cooker mode specified.\n", 0, 0, 0);
        return 255;
    }
    if (!known_mode(opt.mode)) {
        log_msg("Unrecognized cooker mode.\n", 0, 0, 0);
        return 255;
    }
    default_pantry(&opt);
    for (i = 0; i < opt.npantry; i++) {
        if (!is_dir(opt.pantry[i])) {
            log_msg("Directory '%s' does not exist!  Who moved the pantry?\n", opt.pantry[i], 0, 0);
            return 255;
        }
    }
    if (!default_config(&opt))
        return 255;
    if (!opt.nfiles) {
        log_msg("Unable to find any files to cook!\n", 0, 0, 0);
        return 255;
    }
    if (!open_log(&opt))
        return 255;
    log_msg("Starting AssetCooker...\n");
    log_msg("Cook platform %s  mode %s\n", opt.platform, opt.mode);
    for (i = 0; i < opt.nfiles; i++) {
        int ok;
        if (!strcmp(opt.mode, "XLP"))
            ok = cook_xlp(&opt, opt.files[i]);
        else
            ok = cook_artdef(&opt, opt.files[i]);
        if (!ok) {
            failed = 1;
            log_msg("Aborting cook...\n", 0, 0, 0);
        }
    }
    if (failed) {
        log_msg("Cook failed!\n");
        if (g_log)
            fclose(g_log);
        return 255;
    }
    log_msg("Cook completed in %.3f seconds\n", (double)(clock() - t0) / CLOCKS_PER_SEC);
    if (g_log)
        fclose(g_log);
    return 0;
}
