#include "be_object.h"
#include "be_mem.h"
#include <string.h>

#if BE_USE_JSON_MODULE

#define MAX_INDENT      24
#define INDENT_WIDTH    2
#define INDENT_CHAR     ' '

static const char* parser_value(bvm *vm, const char *json);
static void value_dump(bvm *vm, int *indent, int idx, int fmt);

static const char* skip_space(const char *s)
{
    int c;
    while (((c = *s) != '\0') && ((c == ' ')
        || (c == '\t') || (c == '\r') || (c == '\n'))) {
        ++s;
    }
    return s;
}

static int is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static const char* match_char(const char *json, int ch)
{
    json = skip_space(json);
    if (*json == ch) {
        return skip_space(json + 1);
    }
    return NULL;
}

static int is_object(bvm *vm, const char *class, int idx)
{
    if (be_isinstance(vm, idx)) {
        const char *name = be_classname(vm, idx);
        return !strcmp(name, class);
    }
    return  0;
}

static int json_strlen(const char *json)
{
    int ch;
    const char *s = json + 1; /* skip '"' */
    /* get string length "(\\.|[^"])*" */
    while ((ch = *s) != '\0' && ch != '"') {
        ++s;
        if (ch == '\\') {
            ch = *s++;
            if (ch == '\0') {
                return -1;
            }
        }
    }
    return ch ? cast_int(s - json - 1) : -1;
}

static void json2berry(bvm *vm, const char *class)
{
    be_getbuiltin(vm, class);
    be_pushvalue(vm, -2);
    be_call(vm, 1);
    be_moveto(vm, -2, -3);
    be_pop(vm, 2);
}

static const char* parser_true(bvm *vm, const char *json)
{
    if (!strncmp(json, "true", 4)) {
        be_pushbool(vm, btrue);
        return json + 4;
    }
    return NULL;
}

static const char* parser_false(bvm *vm, const char *json)
{
    if (!strncmp(json, "false", 5)) {
        be_pushbool(vm, bfalse);
        return json + 5;
    }
    return NULL;
}

static const char* parser_null(bvm *vm, const char *json)
{
    if (!strncmp(json, "null", 4)) {
        be_pushnil(vm);
        return json + 4;
    }
    return NULL;
}

static char* load_unicode(char *dst, const char *json)
{
    int ucode = 0, i = 4;
    while (i--) {
        int ch = *json++;
        if (ch >= '0' && ch <= '9') {
            ucode = (ucode << 4) | (ch - '0');
        } else if (ch >= 'A' && ch <= 'F') {
            ucode = (ucode << 4) | (ch - 'A' + 0x0A);
        } else if (ch >= 'a' && ch <= 'f') {
            ucode = (ucode << 4) | (ch - 'a' + 0x0A);
        } else {
            return NULL;
        }
    }
    /* convert unicode to utf8 */
    if (ucode < 0x007F) {
        /* unicode: 0000 - 007F -> utf8: 0xxxxxxx */
        *dst++ = (char)(ucode & 0x7F);
    } else if (ucode < 0x7FF) {
        /* unicode: 0080 - 07FF -> utf8: 110xxxxx 10xxxxxx */
        *dst++ = (char)(((ucode >> 6) & 0x1F) | 0xC0);
        *dst++ = (char)((ucode & 0x3F) | 0x80);
    } else {
        /* unicode: 0800 - FFFF -> utf8: 1110xxxx 10xxxxxx 10xxxxxx */
        *dst++ = (char)(((ucode >> 12) & 0x0F) | 0xE0);
        *dst++ = (char)(((ucode >> 6) & 0x03F) | 0x80);
        *dst++ = (char)((ucode & 0x3F) | 0x80);
    }
    return dst;
}

static const char* parser_string(bvm *vm, const char *json)
{
    if (*json == '"') {
        int len = json_strlen(json++);
        if (len > -1) {
            int ch;
            char *buf, *dst = buf = be_malloc(vm, len);
            while ((ch = *json) != '\0' && ch != '"') {
                ++json;
                if (ch == '\\') {
                    ch = *json++; /* skip '\' */
                    switch (ch) {
                    case '"': *dst++ = '"'; break;
                    case '\\': *dst++ = '\\'; break;
                    case '/': *dst++ = '/'; break;
                    case 'b': *dst++ = '\b'; break;
                    case 'f': *dst++ = '\f'; break;
                    case 'n': *dst++ = '\n'; break;
                    case 'r': *dst++ = '\r'; break;
                    case 't': *dst++ = '\t'; break;
                    case 'u': { /* load unicode */
                        dst = load_unicode(dst, json);
                        if (dst == NULL) {
                            be_free(vm, buf, len);
                            return NULL;
                        }
                        json += 4;
                        break;
                    }
                    default: be_free(vm, buf, len); return NULL; /* error */
                    }
                } else {
                    *dst++ = (char)ch;
                }
            }
            if (ch == '"') {
                be_pushnstring(vm, buf, cast_int(dst - buf));
                be_free(vm, buf, len);
                return json + 1; /* skip '"' */
            }
            be_free(vm, buf, len);
        }
    }
    return NULL;
}

static const char* parser_field(bvm *vm, const char *json)
{
    if (json && *json == '"') {
        json = parser_string(vm, json);
        if (json) {
            json = match_char(json, ':');
            if (json) {
                json = parser_value(vm, json);
                if (json) {
                    be_data_insert(vm, -3);
                    be_pop(vm, 2); /* pop key and value */
                    return json;
                }
            }
            be_pop(vm, 1); /* pop key */
        }
    }
    return NULL;
}

static const char* parser_object(bvm *vm, const char *json)
{
    json = match_char(json, '{');
    be_newmap(vm);
    if (*json != '}') {
        const char *s;
        json = parser_field(vm, json);
        if (json == NULL) {
            be_pop(vm, 1); /* pop map */
            return NULL;
        }
        while ((s = match_char(json, ',')) != NULL) {
            json = parser_field(vm, s);
            if (json == NULL) {
                be_pop(vm, 1); /* pop map */
                return NULL;
            }
        }
    }
    if ((json = match_char(json, '}')) == NULL) {
        be_pop(vm, 1); /* pop map */
        return NULL;
    }
    json2berry(vm, "map");
    return json;
}

static const char* parser_array(bvm *vm, const char *json)
{
    json = match_char(json, '[');
    be_newlist(vm);
    if (*json != ']') {
        const char *s;
        json = parser_value(vm, json);
        if (json == NULL) {
            be_pop(vm, 1); /* pop map */
            return NULL;
        }
        be_data_push(vm, -2);
        be_pop(vm, 1); /* pop value */
        while ((s = match_char(json, ',')) != NULL) {
            json = parser_value(vm, s);
            if (json == NULL) {
                be_pop(vm, 1); /* pop map */
                return NULL;
            }
            be_data_push(vm, -2);
            be_pop(vm, 1); /* pop value */
        }
    }
    if ((json = match_char(json, ']')) == NULL) {
        be_pop(vm, 1); /* pop map */
        return NULL;
    }
    json2berry(vm, "list");
    return json;
}

/* parser json value */
static const char* parser_value(bvm *vm, const char *json)
{
    json = skip_space(json);
    switch (*json) {
    case '{': /* object */
        return parser_object(vm, json);
    case '[': /* array */
        return parser_array(vm, json);
    case '"': /* string */
        return parser_string(vm, json);
    case 't': /* true */
        return parser_true(vm, json);
    case 'f': /* false */
        return parser_false(vm, json);
    case 'n': /* null */
        return parser_null(vm, json);
    default: /* number */
        if (*json == '-' || is_digit(*json)) {
            /* check invalid JSON syntax: 0\d+ */
            if (json[0] == '0' && is_digit(json[1])) {
                return NULL;
            }
            return be_str2num(vm, json);
        }
    }
    return NULL;
}

static int m_json_load(bvm *vm)
{
    if (be_isstring(vm, 1)) {
        const char *json = be_tostring(vm, 1);
        json = parser_value(vm, json);
        if (json != NULL && *json == '\0') {
            be_return(vm);
        }
    }
    be_return_nil(vm);
}

static void make_indent(bvm *vm, int stridx, int indent)
{
    if (indent) {
        char buf[MAX_INDENT * INDENT_WIDTH + 1];
        indent = (indent < MAX_INDENT ? indent : MAX_INDENT) * INDENT_WIDTH;
        memset(buf, INDENT_CHAR, indent);
        buf[indent] = '\0';
        stridx = be_absindex(vm, stridx);
        be_pushstring(vm, buf);
        be_strconcat(vm, stridx);
        be_pop(vm, 1);
    }
}

static unsigned escape_length(const char *s)
{
    unsigned c, len = 0;
    for (; (c = *s) != '\0'; ++s) {
        switch (c) {
        case '\"': case '\\': case '\b': case '\f':
        case '\n': case '\r': case '\t':
            len += 1;
            break;
        default:
            if (c < 0x20) {
                len += 5;
            }
            break;
        }
    }
    return len;
}

static unsigned eschex(unsigned num)
{
    return num <= 9 ? '0' + num : 'a' + num - 10;
}

/* escape as JSON */
static char* escape(char *q, unsigned c)
{
    switch (c) {
    case '\"': *q++ = '\\'; *q = '"'; break;
    case '\\': *q++ = '\\'; *q = '\\'; break;
    case '\b': *q++ = '\\'; *q = 'b'; break;
    case '\f': *q++ = '\\'; *q = 'f'; break;
    case '\n': *q++ = '\\'; *q = 'n'; break;
    case '\r': *q++ = '\\'; *q = 'r'; break;
    case '\t': *q++ = '\\'; *q = 't'; break;
    default:
        if (c < 0x20) { /* other characters are escaped using '\uxxxx' */
            *q++ = '\\'; *q++ = 'u';
            *q++ = '0'; *q++ = '0';
            *q++ = (char)eschex(c >> 4);
            *q = (char)eschex(c & 0x0F);
        } else { /* unescaped characters */
            *q = (char)c;
        }
        break;
    }
    return q;
}

static void string_dump(bvm *vm, int idx)
{
    char *buf, *q;
    const char *p;
    const char *s = be_tostring(vm, idx);
    size_t len = (size_t)be_strlen(vm, idx) + 2;
    len += escape_length(s); /* get escaped length */
    buf = q = be_pushbuffer(vm, len);
    *q++ = '"'; /* add first '"' */
    /* generate JSON string */
    for (p = s; *p != '\0'; ++p, ++q) {
        q = escape(q, *p);
    }
    *q = '"'; /* add last '"' */
    be_pushnstring(vm, buf, len); /* make JSON string from buffer */
    be_remove(vm, -2); /* remove buffer */
}

static void object_dump(bvm *vm, int *indent, int idx, int fmt)
{
    be_getmember(vm, idx, ".data");
    be_pushstring(vm, fmt ? "{\n" : "{");
    be_pushiter(vm, -2); /* map iterator use 1 register */
    *indent += fmt;
    while (be_iter_hasnext(vm, -3)) {
        make_indent(vm, -2, fmt ? *indent : 0);
        be_iter_next(vm, -3);
        /* key.tostring() */
        string_dump(vm, -2);
        be_strconcat(vm, -5);
        be_pop(vm, 1);
        be_pushstring(vm, fmt ? ": " : ":"); /* add ': ' */
        be_strconcat(vm, -5);
        be_pop(vm, 1);
        /* value.tostring() */
        value_dump(vm, indent, -1, fmt);
        be_strconcat(vm, -5);
        be_pop(vm, 3);
        if (be_iter_hasnext(vm, -3)) {
            be_pushstring(vm, fmt ? ",\n" : ",");
            be_strconcat(vm, -3);
            be_pop(vm, 1);
        } else if (fmt) {
            be_pushstring(vm, "\n");
            be_strconcat(vm, -3);
            be_pop(vm, 1);
        }
    }
    *indent -= fmt;
    be_pop(vm, 1); /* pop iterator */
    make_indent(vm, -1,  fmt ? *indent : 0);
    be_pushstring(vm, "}");
    be_strconcat(vm, -2);
    be_moveto(vm, -2, -3);
    be_pop(vm, 2);
}

static void array_dump(bvm *vm, int *indent, int idx, int fmt)
{
    be_getmember(vm, idx, ".data");
    be_pushstring(vm, fmt ? "[\n" : "[");
    be_pushiter(vm, -2);
    *indent += fmt;
    while (be_iter_hasnext(vm, -3)) {
        make_indent(vm, -2,  fmt ? *indent : 0);
        be_iter_next(vm, -3);
        value_dump(vm, indent, -1, fmt);
        be_strconcat(vm, -4);
        be_pop(vm, 2);
        if (be_iter_hasnext(vm, -3)) {
            be_pushstring(vm, fmt ? ",\n" : ",");
            be_strconcat(vm, -3);
            be_pop(vm, 1);
        } else if (fmt) {
            be_pushstring(vm, "\n");
            be_strconcat(vm, -3);
            be_pop(vm, 1);
        }
    }
    *indent -= fmt;
    be_pop(vm, 1); /* pop iterator */
    make_indent(vm, -1,  fmt ? *indent : 0);
    be_pushstring(vm, "]");
    be_strconcat(vm, -2);
    be_moveto(vm, -2, -3);
    be_pop(vm, 2);
}

static void value_dump(bvm *vm, int *indent, int idx, int fmt)
{
    if (is_object(vm, "map", idx)) { /* convert to json object */
        object_dump(vm, indent, idx, fmt);
    } else if (is_object(vm, "list", idx)) { /* convert to json array */
        array_dump(vm, indent, idx, fmt);
    } else if (be_isnil(vm, idx)) { /* convert to json null */
        be_pushstring(vm, "null");
    } else if (be_isnumber(vm, idx) || be_isbool(vm, idx)) { /* convert to json number and boolean */
        be_tostring(vm, idx);
        be_pushvalue(vm, idx); /* push to top */
    } else { /* convert to string */
        string_dump(vm, idx);
    }
}

static int m_json_dump(bvm *vm)
{
    int indent = 0, argc = be_top(vm);
    int fmt = 0;
    if (argc > 1) {
        fmt = !strcmp(be_tostring(vm, 2), "format");
    }
    value_dump(vm, &indent, 1, fmt);
    be_return(vm);
}

#if !BE_USE_PRECOMPILED_OBJECT
be_native_module_attr_table(json) {
    be_native_module_function("load", m_json_load),
    be_native_module_function("dump", m_json_dump)
};

be_define_native_module(json, NULL);
#else
/* @const_object_info_begin
module json (scope: global, depend: BE_USE_JSON_MODULE) {
    load, func(m_json_load)
    dump, func(m_json_dump)
}
@const_object_info_end */
#include "../generate/be_fixed_json.h"
#endif

#endif /* BE_USE_JSON_MODULE */
