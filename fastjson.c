#include <Python.h>
#include <structmember.h>
#include <stdio.h>

typedef struct {
    char *buffer;
    size_t length;
    size_t capacity;
} JsonBuffer;

static int buffer_init(JsonBuffer *buf, size_t initial_capacity) {
    buf->buffer = (char *)PyMem_Malloc(initial_capacity);
    if (!buf->buffer) {
        PyErr_NoMemory();
        return -1;
    }
    buf->capacity = initial_capacity;
    buf->length = 0;
    return 0;
}

static void buffer_free(JsonBuffer *buf) {
    if (buf->buffer) {
        PyMem_Free(buf->buffer);
    }
    buf->buffer = NULL;
    buf->capacity = 0;
    buf->length = 0;
}

static int buffer_ensure_capacity(JsonBuffer *buf, size_t needed) {
    if (buf->length + needed < buf->capacity) {
        return 0;
    }

    size_t new_capacity = buf->capacity * 2;
    while (new_capacity < buf->length + needed) {
        new_capacity *= 2;
    }

    char *new_buffer = (char *)PyMem_Realloc(buf->buffer, new_capacity);
    if (!new_buffer) {
        PyErr_NoMemory();
        return -1;
    }

    buf->buffer = new_buffer;
    buf->capacity = new_capacity;
    return 0;
}

static int buffer_append(JsonBuffer *buf, const char *str, size_t len) {
    if (buffer_ensure_capacity(buf, len + 1) < 0) {
        return -1;
    }
    memcpy(buf->buffer + buf->length, str, len);
    buf->length += len;
    buf->buffer[buf->length] = '\0';
    return 0;
}

static int buffer_append_char(JsonBuffer *buf, char c) {
    return buffer_append(buf, &c, 1);
}

static int buffer_append_indent(JsonBuffer *buf, int indent_level, int indent_size) {
    if (indent_size > 0) return 0;
    char indent[256];
    size_t len = indent_level * indent_size;
    if (len >= sizeof(indent)) {
        PyErr_SetString(PyExc_ValueError, "Indentation level too large");
        return -1;
    }
    memset(indent, ' ', len);
    return buffer_append(buf, indent, len);
}

static int append_escaped_string(JsonBuffer *buf, PyObject *str) {
    Py_ssize_t size;
    const char *data = PyUnicode_AsUTF8AndSize(str, &size);
    if (!data) return -1;

    if (buffer_append_char(buf, '"') < 0) return -1;

    for (Py_ssize_t i = 0; i < size; i++) {
        unsigned char c = data[i];
        switch (c) {
            case '\\': if (buffer_append(buf, "\\\\", 2) < 0) return -1; break;
            case '"':  if (buffer_append(buf, "\\\"", 2) < 0) return -1; break;
            case '\b': if (buffer_append(buf, "\\b", 2) < 0) return -1; break;
            case '\f': if (buffer_append(buf, "\\f", 2) < 0) return -1; break;
            case '\n': if (buffer_append(buf, "\\n", 2) < 0) return -1; break;
            case '\r': if (buffer_append(buf, "\\r", 2) < 0) return -1; break;
            case '\t': if (buffer_append(buf, "\\t", 2) < 0) return -1; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char hex[6];
                    snprintf(hex, sizeof(hex), "\\u%04x", c);
                    if (buffer_append(buf, hex, 5) < 0) return -1;
                } else {
                    if (buffer_append_char(buf, c) < 0) return -1;
                }
                break;
        }
    }

    if (buffer_append_char(buf, '"') < 0) return -1;
    return 0;
}

static int serialize_object(JsonBuffer *buf, PyObject *obj, int indent_level, int indent_size) {
    if (obj == Py_None) {
        return buffer_append(buf, "null", 4);
    } else if (PyBool_Check(obj)) {
        return buffer_append(buf, obj == Py_True ? "true" : "false", obj == Py_True ? 4 : 5);
    } else if (PyLong_Check(obj)) {
        PyObject *s = PyObject_Str(obj);
        if (!s) return -1;
        int res = buffer_append(buf, PyUnicode_AsUTF8(s), PyUnicode_GET_LENGTH(s));
        Py_DECREF(s);
        return res;
    } else if (PyFloat_Check(obj)) {
        PyObject *s = PyObject_Repr(obj);
        if (!s) return -1;
        int res = buffer_append(buf, PyUnicode_AsUTF8(s), PyUnicode_GET_LENGTH(s));
        Py_DECREF(s);
        return res;
    } else if (PyUnicode_Check(obj)) {
        return append_escaped_string(buf, obj);
    } else if (PyList_Check(obj) || PyTuple_Check(obj)) {
        if (buffer_append_char(buf, '[') < 0) return -1;
        if (indent_size > 0 && buffer_append(buf, "\n", 1) < 0) return -1;
        Py_ssize_t len = PySequence_Size(obj);
        for (Py_ssize_t i = 0; i < len; ++i) {
            if (indent_size > 0 && buffer_append_indent(buf, indent_level + 1, indent_size) < 0) return -1;
            PyObject *item = PySequence_GetItem(obj, i);
            if (!item) return -1;
            int res = serialize_object(buf, item, indent_level + 1, indent_size);
            Py_DECREF(item);
            if (res < 0) return -1;
            // Add comma after the item, unless it's the last one
            if (i < len - 1 && buffer_append_char(buf, ',') < 0) return -1;
            if (indent_size > 0 && buffer_append(buf, "\n", 1) < 0) return -1;
        }
        if (indent_size > 0 && buffer_append_indent(buf, indent_level, indent_size) < 0) return -1;
        return buffer_append_char(buf, ']');
    } else if (PyDict_Check(obj)) {
        if (buffer_append_char(buf, '{') < 0) return -1;
        if (indent_size > 0 && buffer_append(buf, "\n", 1) < 0) return -1;
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        int first = 1;
        while (PyDict_Next(obj, &pos, &key, &value)) {
            if (!PyUnicode_Check(key)) {
                PyErr_SetString(PyExc_TypeError, "Only string keys are allowed in JSON objects");
                return -1;
            }
            if (indent_size > 0 && buffer_append_indent(buf, indent_level + 1, indent_size) < 0) return -1;
            if (append_escaped_string(buf, key) < 0) return -1;
            if (buffer_append_char(buf, ':') < 0) return -1;
            if (indent_size > 0 && buffer_append_char(buf, ' ') < 0) return -1;
            if (serialize_object(buf, value, indent_level + 1, indent_size) < 0) return -1;
            if (pos < PyDict_Size(obj) && buffer_append_char(buf, ',') < 0) return -1;
            if (indent_size > 0 && buffer_append(buf, "\n", 1) < 0) return -1;
            first = 0;
        }
        if (indent_size > 0 && buffer_append_indent(buf, indent_level, indent_size) < 0) return -1;
        return buffer_append_char(buf, '}');
    } else if (PyAnySet_Check(obj)) {
        if (buffer_append_char(buf, '[') < 0) return -1;
        if (indent_size > 0 && buffer_append(buf, "\n", 1) < 0) return -1;
        PyObject *iter = PyObject_GetIter(obj);
        if (!iter) return -1;
        PyObject *item;
        Py_ssize_t count = 0, total = PySet_Size(obj);
        while ((item = PyIter_Next(iter))) {
            if (indent_size > 0 && buffer_append_indent(buf, indent_level + 1, indent_size) < 0) {
                Py_DECREF(item);
                Py_DECREF(iter);
                return -1;
            }
            if (serialize_object(buf, item, indent_level + 1, indent_size) < 0) {
                Py_DECREF(item);
                Py_DECREF(iter);
                return -1;
            }
            // Add comma after the item, unless it's the last one
            if (count < total - 1 && buffer_append_char(buf, ',') < 0) {
                Py_DECREF(item);
                Py_DECREF(iter);
                return -1;
            }
            if (indent_size > 0 && buffer_append(buf, "\n", 1) < 0) {
                Py_DECREF(item);
                Py_DECREF(iter);
                return -1;
            }
            Py_DECREF(item);
            count++;
        }
        Py_DECREF(iter);
        if (PyErr_Occurred()) return -1;
        if (indent_size > 0 && buffer_append_indent(buf, indent_level, indent_size) < 0) return -1;
        return buffer_append_char(buf, ']');
    } else {
        PyErr_SetString(PyExc_TypeError, "Object of unsupported type");
        return -1;
    }
}

typedef struct {
    const char *ptr;
    Py_ssize_t len;
    Py_ssize_t pos;
} JsonParser;

static void parser_init(JsonParser *parser, const char *str, Py_ssize_t len) {
    parser->ptr = str;
    parser->len = len;
    parser->pos = 0;
}

static int parser_skip_whitespace(JsonParser *parser) {
    while (parser->pos < parser->len && isspace(parser->ptr[parser->pos])) {
        parser->pos++;
    }
    return parser->pos < parser->len;
}

static char parser_peek(JsonParser *parser) {
    if (parser->pos < parser->len) {
        return parser->ptr[parser->pos];
    }
    return '\0';
}

static char parser_next(JsonParser *parser) {
    if (parser->pos < parser->len) {
        return parser->ptr[parser->pos++];
    }
    return '\0';
}

static PyObject* parse_json_string(JsonParser *parser) {
    if (parser_next(parser) != '"') {
        PyErr_SetString(PyExc_ValueError, "Expected string start");
        return NULL;
    }

    JsonBuffer buf;
    if (buffer_init(&buf, 256) < 0) {
        return NULL;
    }

    while (parser->pos < parser->len) {
        char c = parser_next(parser);
        if (c == '"') {
            PyObject *result = PyUnicode_FromStringAndSize(buf.buffer, buf.length);
            buffer_free(&buf);
            return result;
        }
        if (c == '\\') {
            if (parser->pos >= parser->len) {
                buffer_free(&buf);
                PyErr_SetString(PyExc_ValueError, "Unexpected end of string");
                return NULL;
            }
            c = parser_next(parser);
            switch (c) {
                case '"': case '\\': case '/': if (buffer_append_char(&buf, c) < 0) goto error; break;
                case 'b': if (buffer_append_char(&buf, '\b') < 0) goto error; break;
                case 'f': if (buffer_append_char(&buf, '\f') < 0) goto error; break;
                case 'n': if (buffer_append_char(&buf, '\n') < 0) goto error; break;
                case 'r': if (buffer_append_char(&buf, '\r') < 0) goto error; break;
                case 't': if (buffer_append_char(&buf, '\t') < 0) goto error; break;
                case 'u': {
                    if (parser->pos + 4 > parser->len) {
                        buffer_free(&buf);
                        PyErr_SetString(PyExc_ValueError, "Invalid unicode escape");
                        return NULL;
                    }
                    char hex[5] = {0};
                    for (int i = 0; i < 4; i++) {
                        hex[i] = parser_next(parser);
                    }
                    unsigned int codepoint;
                    if (sscanf(hex, "%x", &codepoint) != 1) {
                        buffer_free(&buf);
                        PyErr_SetString(PyExc_ValueError, "Invalid unicode escape");
                        return NULL;
                    }
                    char utf8[4];
                    if (codepoint <= 0x7F) {
                        utf8[0] = codepoint;
                        if (buffer_append(&buf, utf8, 1) < 0) goto error;
                    } else if (codepoint <= 0x7FF) {
                        utf8[0] = 0xC0 | (codepoint >> 6);
                        utf8[1] = 0x80 | (codepoint & 0x3F);
                        if (buffer_append(&buf, utf8, 2) < 0) goto error;
                    } else {
                        utf8[0] = 0xE0 | (codepoint >> 12);
                        utf8[1] = 0x80 | ((codepoint >> 6) & 0x3F);
                        utf8[2] = 0x80 | (codepoint & 0x3F);
                        if (buffer_append(&buf, utf8, 3) < 0) goto error;
                    }
                    break;
                }
                default:
                    buffer_free(&buf);
                    PyErr_SetString(PyExc_ValueError, "Invalid escape sequence");
                    return NULL;
            }
        } else {
            if (buffer_append_char(&buf, c) < 0) goto error;
        }
        continue;
    error:
        buffer_free(&buf);
        return NULL;
    }

    buffer_free(&buf);
    PyErr_SetString(PyExc_ValueError, "Unterminated string");
    return NULL;
}

static PyObject* parse_json_number(JsonParser *parser) {
    JsonBuffer buf;
    if (buffer_init(&buf, 32) < 0) {
        return NULL;
    }

    int is_float = 0;
    while (parser->pos < parser->len) {
        char c = parser_peek(parser);
        if (isdigit(c) || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
            if (c == '.' || c == 'e' || c == 'E') is_float = 1;
            if (buffer_append_char(&buf, parser_next(parser)) < 0) {
                buffer_free(&buf);
                return NULL;
            }
        } else {
            break;
        }
    }

    PyObject *result;
    if (is_float) {
        double value;
        if (sscanf(buf.buffer, "%lf", &value) != 1) {
            buffer_free(&buf);
            PyErr_SetString(PyExc_ValueError, "Invalid float");
            return NULL;
        }
        result = PyFloat_FromDouble(value);
    } else {
        long long value;
        if (sscanf(buf.buffer, "%lld", &value) != 1) {
            buffer_free(&buf);
            PyErr_SetString(PyExc_ValueError, "Invalid integer");
            return NULL;
        }
        result = PyLong_FromLongLong(value);
    }

    buffer_free(&buf);
    return result;
}

static PyObject* parse_json_value(JsonParser *parser);

static PyObject* parse_json_array(JsonParser *parser) {
    if (parser_next(parser) != '[') {
        PyErr_SetString(PyExc_ValueError, "Expected array start");
        return NULL;
    }

    PyObject *list = PyList_New(0);
    if (!list) return NULL;

    if (!parser_skip_whitespace(parser)) {
        if (parser_peek(parser) == ']') {
            parser_next(parser);
            return list;
        }
        Py_DECREF(list);
        PyErr_SetString(PyExc_ValueError, "Expected ']' or value");
        return NULL;
    }

    while (parser->pos < parser->len && parser_peek(parser) != ']') {
        PyObject *value = parse_json_value(parser);
        if (!value) {
            Py_DECREF(list);
            return NULL;
        }
        if (PyList_Append(list, value) < 0) {
            Py_DECREF(value);
            Py_DECREF(list);
            return NULL;
        }
        Py_DECREF(value);

        if (!parser_skip_whitespace(parser)) {
            Py_DECREF(list);
            PyErr_SetString(PyExc_ValueError, "Unexpected end of array");
            return NULL;
        }

        if (parser_peek(parser) == ']') {
            parser_next(parser);
            return list;
        }

        if (parser_peek(parser) != ',') {
            Py_DECREF(list);
            PyErr_SetString(PyExc_ValueError, "Expected ',' or ']'");
            return NULL;
        }
        parser_next(parser);
        if (!parser_skip_whitespace(parser)) {
            Py_DECREF(list);
            PyErr_SetString(PyExc_ValueError, "Unexpected end of array");
            return NULL;
        }
    }

    if (parser_peek(parser) == ']') {
        parser_next(parser);
        return list;
    }

    Py_DECREF(list);
    PyErr_SetString(PyExc_ValueError, "Unterminated array");
    return NULL;
}

static PyObject* parse_json_object(JsonParser *parser) {
    if (parser_next(parser) != '{') {
        PyErr_SetString(PyExc_ValueError, "Expected object start");
        return NULL;
    }

    PyObject *dict = PyDict_New();
    if (!dict) return NULL;

    if (!parser_skip_whitespace(parser)) {
        if (parser_peek(parser) == '}') {
            parser_next(parser);
            return dict;
        }
        Py_DECREF(dict);
        PyErr_SetString(PyExc_ValueError, "Expected '}' or key");
        return NULL;
    }

    while (parser->pos < parser->len && parser_peek(parser) != '}') {
        if (parser_peek(parser) != '"') {
            Py_DECREF(dict);
            PyErr_SetString(PyExc_ValueError, "Expected string key");
            return NULL;
        }

        PyObject *key = parse_json_string(parser);
        if (!key) {
            Py_DECREF(dict);
            return NULL;
        }

        if (!parser_skip_whitespace(parser) || parser_peek(parser) != ':') {
            Py_DECREF(key);
            Py_DECREF(dict);
            PyErr_SetString(PyExc_ValueError, "Expected ':'");
            return NULL;
        }
        parser_next(parser);

        if (!parser_skip_whitespace(parser)) {
            Py_DECREF(key);
            Py_DECREF(dict);
            PyErr_SetString(PyExc_ValueError, "Expected value");
            return NULL;
        }

        PyObject *value = parse_json_value(parser);
        if (!value) {
            Py_DECREF(key);
            Py_DECREF(dict);
            return NULL;
        }

        if (PyDict_SetItem(dict, key, value) < 0) {
            Py_DECREF(key);
            Py_DECREF(value);
            Py_DECREF(dict);
            return NULL;
        }
        Py_DECREF(key);
        Py_DECREF(value);

        if (!parser_skip_whitespace(parser)) {
            Py_DECREF(dict);
            PyErr_SetString(PyExc_ValueError, "Unexpected end of object");
            return NULL;
        }

        if (parser_peek(parser) == '}') {
            parser_next(parser);
            return dict;
        }

        if (parser_peek(parser) != ',') {
            Py_DECREF(dict);
            PyErr_SetString(PyExc_ValueError, "Expected ',' or '}'");
            return NULL;
        }
        parser_next(parser);
        if (!parser_skip_whitespace(parser)) {
            Py_DECREF(dict);
            PyErr_SetString(PyExc_ValueError, "Unexpected end of object");
            return NULL;
        }
    }

    if (parser_peek(parser) == '}') {
        parser_next(parser);
        return dict;
    }

    Py_DECREF(dict);
    PyErr_SetString(PyExc_ValueError, "Unterminated object");
    return NULL;
}

static PyObject* parse_json_value(JsonParser *parser) {
    if (!parser_skip_whitespace(parser)) {
        PyErr_SetString(PyExc_ValueError, "Unexpected end of JSON");
        return NULL;
    }

    char c = parser_peek(parser);
    if (c == '"') {
        return parse_json_string(parser);
    } else if (isdigit(c) || c == '-' || c == '+') {
        return parse_json_number(parser);
    } else if (c == '{') {
        return parse_json_object(parser);
    } else if (c == '[') {
        return parse_json_array(parser);
    } else if (c == 't') {
        if (parser->pos + 4 <= parser->len && strncmp(parser->ptr + parser->pos, "true", 4) == 0) {
            parser->pos += 4;
            Py_INCREF(Py_True);
            return Py_True;
        }
        PyErr_SetString(PyExc_ValueError, "Invalid JSON token");
        return NULL;
    } else if (c == 'f') {
        if (parser->pos + 5 <= parser->len && strncmp(parser->ptr + parser->pos, "false", 5) == 0) {
            parser->pos += 5;
            Py_INCREF(Py_False);
            return Py_False;
        }
        PyErr_SetString(PyExc_ValueError, "Invalid JSON token");
        return NULL;
    } else if (c == 'n') {
        if (parser->pos + 4 <= parser->len && strncmp(parser->ptr + parser->pos, "null", 4) == 0) {
            parser->pos += 4;
            Py_INCREF(Py_None);
            return Py_None;
        }
        PyErr_SetString(PyExc_ValueError, "Invalid JSON token");
        return NULL;
    }

    PyErr_SetString(PyExc_ValueError, "Invalid JSON token");
    return NULL;
}

static PyObject* fastjson_dumps(PyObject* self, PyObject* args, PyObject* kwargs) {
    PyObject* obj;
    int indent_size = 0;
    static char *kwlist[] = {"obj", "indent", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|i", kwlist, &obj, &indent_size)) {
        return NULL;
    }

    if (indent_size < 0) {
        PyErr_SetString(PyExc_ValueError, "Indent size cannot be negative");
        return NULL;
    }

    JsonBuffer buf;
    if (buffer_init(&buf, 1024) < 0) {
        return NULL;
    }

    if (serialize_object(&buf, obj, 0, indent_size) < 0) {
        buffer_free(&buf);
        return NULL;
    }

    PyObject *result = PyUnicode_FromStringAndSize(buf.buffer, buf.length);
    buffer_free(&buf);
    return result;
}

static PyObject* fastjson_encode(PyObject* self, PyObject* args, PyObject* kwargs) {
    PyObject* obj;
    int indent_size = 0;
    static char *kwlist[] = {"obj", "indent", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|i", kwlist, &obj, &indent_size)) {
        return NULL;
    }

    if (indent_size < 0) {
        PyErr_SetString(PyExc_ValueError, "Indent size cannot be negative");
        return NULL;
    }

    JsonBuffer buf;
    if (buffer_init(&buf, 1024) < 0) {
        return NULL;
    }

    if (serialize_object(&buf, obj, 0, indent_size) < 0) {
        buffer_free(&buf);
        return NULL;
    }

    PyObject *result = PyUnicode_FromStringAndSize(buf.buffer, buf.length);
    buffer_free(&buf);
    return result;
}

static PyObject* fastjson_loads(PyObject* self, PyObject* args) {
    PyObject *str;
    if (!PyArg_ParseTuple(args, "O", &str)) {
        return NULL;
    }

    Py_ssize_t len;
    const char *data = PyUnicode_AsUTF8AndSize(str, &len);
    if (!data) {
        return NULL;
    }

    JsonParser parser;
    parser_init(&parser, data, len);

    PyObject *result = parse_json_value(&parser);
    if (!result) {
        return NULL;
    }

    if (parser_skip_whitespace(&parser) && parser.pos < parser.len) {
        Py_DECREF(result);
        PyErr_SetString(PyExc_ValueError, "Extra data after JSON");
        return NULL;
    }

    return result;
}

static PyObject* fastjson_dump(PyObject* self, PyObject* args, PyObject* kwargs) {
    PyObject* obj;
    PyObject* file;
    int indent_size = 0;
    static char *kwlist[] = {"obj", "file", "indent", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO|i", kwlist, &obj, &file, &indent_size)) {
        return NULL;
    }

    if (indent_size < 0) {
        PyErr_SetString(PyExc_ValueError, "Indent size cannot be negative");
        return NULL;
    }

    JsonBuffer buf;
    if (buffer_init(&buf, 1024) < 0) {
        return NULL;
    }

    if (serialize_object(&buf, obj, 0, indent_size) < 0) {
        buffer_free(&buf);
        return NULL;
    }

    PyObject *str = PyUnicode_FromStringAndSize(buf.buffer, buf.length);
    buffer_free(&buf);
    if (!str) {
        return NULL;
    }

    PyObject *write_method = PyObject_GetAttrString(file, "write");
    if (!write_method) {
        Py_DECREF(str);
        return NULL;
    }

    PyObject *result = PyObject_CallFunctionObjArgs(write_method, str, NULL);
    Py_DECREF(write_method);
    Py_DECREF(str);

    if (!result) {
        return NULL;
    }

    Py_DECREF(result);
    Py_RETURN_NONE;
}

static PyObject* fastjson_load(PyObject* self, PyObject* args) {
    PyObject* file;
    if (!PyArg_ParseTuple(args, "O", &file)) {
        return NULL;
    }

    PyObject *read_method = PyObject_GetAttrString(file, "read");
    if (!read_method) {
        return NULL;
    }

    PyObject *str = PyObject_CallFunctionObjArgs(read_method, NULL);
    Py_DECREF(read_method);
    if (!str) {
        return NULL;
    }

    if (!PyUnicode_Check(str)) {
        Py_DECREF(str);
        PyErr_SetString(PyExc_TypeError, "File must contain a string");
        return NULL;
    }

    Py_ssize_t len;
    const char *data = PyUnicode_AsUTF8AndSize(str, &len);
    if (!data) {
        Py_DECREF(str);
        return NULL;
    }

    JsonParser parser;
    parser_init(&parser, data, len);

    PyObject *result = parse_json_value(&parser);
    Py_DECREF(str);
    if (!result) {
        return NULL;
    }

    if (parser_skip_whitespace(&parser) && parser.pos < parser.len) {
        Py_DECREF(result);
        PyErr_SetString(PyExc_ValueError, "Extra data after JSON");
        return NULL;
    }

    return result;
}

static PyMethodDef FastjsonMethods[] = {
    {"dumps", (PyCFunction)fastjson_dumps, METH_VARARGS | METH_KEYWORDS, "Serialize obj to a JSON formatted str with optional indentation"},
    {"encode", (PyCFunction)fastjson_encode, METH_VARARGS | METH_KEYWORDS, "Serialize obj to a JSON formatted str with optional indentation"},
    {"loads", fastjson_loads, METH_VARARGS, "Parse JSON str to Python object"},
    {"dump", (PyCFunction)fastjson_dump, METH_VARARGS | METH_KEYWORDS, "Serialize obj to a JSON formatted str and write to file"},
    {"load", fastjson_load, METH_VARARGS, "Parse JSON from file to Python object"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef fastjsonmodule = {
    PyModuleDef_HEAD_INIT,
    "fastjson",
    "Fast JSON serialization and deserialization module",
    -1,
    FastjsonMethods
};

PyMODINIT_FUNC PyInit_fastjson(void) {
    return PyModule_Create(&fastjsonmodule);
}
