/* builder.c
 * Copyright (c) 2011, 2016 Peter Ohler
 * All rights reserved.
 */

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ox.h"
#include "buf.h"
#include "err.h"

#define MAX_DEPTH	128

typedef struct _Element {
    char	*name;
    char	buf[64];
    int		len;
    bool	has_child;
    bool	non_text_child;
} *Element;

typedef struct _Builder {
    struct _Buf		buf;
    int			indent;
    char		encoding[64];
    int			depth;
    FILE		*file;
    struct _Element	stack[MAX_DEPTH];
} *Builder;

static VALUE		builder_class = Qundef;
static const char	indent_spaces[] = "\n                                                                                                                                "; // 128 spaces

// The : character is equivalent to 10. Used for replacement characters up to 10
// characters long such as '&#x10FFFF;'.
static char	xml_friendly_chars[257] = "\
:::::::::11::1::::::::::::::::::\
11611156111111111111111111114141\
11111111111111111111111111111111\
11111111111111111111111111111111\
11111111111111111111111111111111\
11111111111111111111111111111111\
11111111111111111111111111111111\
11111111111111111111111111111111";

inline static size_t
xml_str_len(const unsigned char *str, size_t len) {
    size_t	size = 0;

    for (; 0 < len; str++, len--) {
	size += xml_friendly_chars[*str];
    }
    return size - len * (size_t)'0';
}

static void
append_indent(Builder b) {
    if (0 == b->indent) {
	return;
    }
    if (b->buf.head < b->buf.tail) {
	int	cnt = (b->indent * (b->depth + 1)) + 1;

	if (sizeof(indent_spaces) <= cnt) {
	    cnt = sizeof(indent_spaces) - 1;
	}
	buf_append_string(&b->buf, indent_spaces, cnt);
    }
}

static void
append_string(Buf b, const char *str, size_t size) {
    size_t	xsize = xml_str_len((const unsigned char*)str, size);

    if (size == xsize) {
	buf_append_string(b, str, size);
    } else {
	char	buf[256];
	char	*end = buf + sizeof(buf) - 1;
	char	*bp = buf;
	int	i = size;
	
	for (; '\0' != *str && 0 < i; i--, str++) {
	    if ('1' == xml_friendly_chars[(unsigned char)*str]) {
		if (end <= bp) {
		    buf_append_string(b, buf, bp - buf);
		    bp = buf;
		}
		*bp++ = *str;
	    } else {
		if (buf < bp) {
		    buf_append_string(b, buf, bp - buf);
		    bp = buf;
		}
		switch (*str) {
		case '"':
		    buf_append_string(b, "&quot;", 6);
		    break;
		case '&':
		    buf_append_string(b, "&amp;", 5);
		    break;
		case '\'':
		    buf_append_string(b, "&apos;", 6);
		    break;
		case '<':
		    buf_append_string(b, "&lt;", 4);
		    break;
		case '>':
		    buf_append_string(b, "&gt;", 4);
		    break;
		default:
		    // Must be one of the invalid characters.
		    rb_raise(rb_eSyntaxError, "'\\#x%02x' is not a valid XML character.", *str);
		    break;
		}
	    }
	}
	if (buf < bp) {
	    buf_append_string(b, buf, bp - buf);
	    bp = buf;
	}
    }
}

static void
append_sym_str(Buf b, VALUE v) {
    const char	*s;
    int		len;
    
    switch (rb_type(v)) {
    case T_STRING:
	s = StringValuePtr(v);
	len = RSTRING_LEN(v);
	break;
    case T_SYMBOL:
	s = rb_id2name(SYM2ID(v));
	len = strlen(s);
	break;
    default:
	rb_raise(ox_arg_error_class, "expected a Symbol or String");
	break;
    }
    append_string(b, s, len);
}

static void
i_am_a_child(Builder b, bool is_text) {
    if (0 <= b->depth) {
	Element	e = &b->stack[b->depth];
	
	if (!e->has_child) {
	    e->has_child = true;
	    buf_append(&b->buf, '>');
	}
	if (!is_text) {
	    e->non_text_child = true;
	}
    }
}

static int
append_attr(VALUE key, VALUE value, Builder b) {
    buf_append(&b->buf, ' ');
    append_sym_str(&b->buf, key);
    buf_append_string(&b->buf, "=\"", 2);
    Check_Type(value, T_STRING);
    buf_append_string(&b->buf, StringValuePtr(value), RSTRING_LEN(value));
    buf_append(&b->buf, '"');
    
    return ST_CONTINUE;
}

static void
init(Builder b, int fd, int indent, long initial_size) {
    buf_init(&b->buf, fd, initial_size);
    b->indent = indent;
    *b->encoding = '\0';
    b->depth = -1;
}

static void
builder_free(void *ptr) {
    Builder	b;
    Element	e;
    int		d;
    
    if (0 == ptr) {
	return;
    }
    b = (Builder)ptr;
    buf_cleanup(&b->buf);
    for (e = b->stack, d = b->depth; 0 < d; d--, e++) {
	if (e->name != e->buf) {
	    free(e->name);
	}
    }
    xfree(ptr);
}

static void
pop(Builder b) {
    Element	e;

    if (0 > b->depth) {
	rb_raise(ox_arg_error_class, "closed to many element");
    }
    e = &b->stack[b->depth];
    b->depth--;
    if (e->has_child) {
	if (e->non_text_child) {
	    append_indent(b);
	}
	buf_append_string(&b->buf, "</", 2);
	buf_append_string(&b->buf, e->name, e->len);
	buf_append(&b->buf, '>');
	if (e->buf != e->name) {
	    free(e->name);
	    e->name = 0;
	}
    } else {
	buf_append_string(&b->buf, "/>", 2);
    }
}

static void
bclose(Builder b) {
    while (0 <= b->depth) {
	pop(b);
    }
    buf_append(&b->buf, '\n');
    buf_finish(&b->buf);
    if (NULL != b->file) {
	fclose(b->file);
    }
}

static VALUE
to_s(Builder b) {
    volatile VALUE	rstr;

    if (0 != b->buf.fd) {
	rb_raise(ox_arg_error_class, "can not create a String with a stream or file builder.");
    }
    if ('\n' != *(b->buf.tail - 1)) {
	buf_append(&b->buf, '\n');
    }
    *b->buf.tail = '\0'; // for debugging
    rstr = rb_str_new(b->buf.head, buf_len(&b->buf));

    if ('\0' != *b->encoding) {
#if HAS_ENCODING_SUPPORT
	rb_enc_associate(rstr, rb_enc_find(b->encoding));
#endif
    }
    return rstr;
}

/* call-seq: new(options)
 *
 * Creates a new Builder that will write to a string that can be retrieved with
 * the to_s() method. If a block is given it is executed with a single parameter
 * which is the builder instance. The return value is then the generated string.
 *
 * - +options+ - (Hash) formating options
 *   - +:indent+ (Fixnum) indentaion level
 *   - +:size+ (Fixnum) the initial size of the string buffer
 */
static VALUE
builder_new(int argc, VALUE *argv, VALUE self) {
    Builder	b = ALLOC(struct _Builder);
    int		indent = ox_default_options.indent;
    long	buf_size = 0;
    
    if (1 == argc) {
	volatile VALUE	v;

	rb_check_type(*argv, T_HASH);
	if (Qnil != (v = rb_hash_lookup(*argv, ox_indent_sym))) {
	    if (rb_cFixnum != rb_obj_class(v)) {
		rb_raise(ox_parse_error_class, ":indent must be a fixnum.\n");
	    }
	    indent = NUM2INT(v);
	}
	if (Qnil != (v = rb_hash_lookup(*argv, ox_size_sym))) {
	    if (rb_cFixnum != rb_obj_class(v)) {
		rb_raise(ox_parse_error_class, ":size must be a fixnum.\n");
	    }
	    buf_size = NUM2LONG(v);
	}
    }
    b->file = NULL;
    init(b, 0, indent, buf_size);

    if (rb_block_given_p()) {
	volatile VALUE	rb = Data_Wrap_Struct(builder_class, NULL, builder_free, b);
	rb_yield(rb);
	bclose(b);

	return to_s(b);
    } else {
	return Data_Wrap_Struct(builder_class, NULL, builder_free, b);
    }
}

/* call-seq: file(filename, options)
 *
 * Creates a new Builder that will write to a file.
 *
 * - +filename+ (String) filename to write to
 * - +options+ - (Hash) formating options
 *   - +:indent+ (Fixnum) indentaion level
 *   - +:size+ (Fixnum) the initial size of the string buffer
 */
static VALUE
builder_file(int argc, VALUE *argv, VALUE self) {
    Builder	b = ALLOC(struct _Builder);
    int		indent = ox_default_options.indent;
    long	buf_size = 0;
    FILE	*f;
    
    if (1 > argc) {
	rb_raise(ox_arg_error_class, "missing filename");
    }
    Check_Type(*argv, T_STRING);
    if (NULL == (f = fopen(StringValuePtr(*argv), "w"))) {
	xfree(b);
	rb_raise(rb_eIOError, "%s\n", strerror(errno));
    }
    if (2 == argc) {
	volatile VALUE	v;

	rb_check_type(argv[1], T_HASH);
	if (Qnil != (v = rb_hash_lookup(argv[1], ox_indent_sym))) {
	    if (rb_cFixnum != rb_obj_class(v)) {
		rb_raise(ox_parse_error_class, ":indent must be a fixnum.\n");
	    }
	    indent = NUM2INT(v);
	}
	if (Qnil != (v = rb_hash_lookup(argv[1], ox_size_sym))) {
	    if (rb_cFixnum != rb_obj_class(v)) {
		rb_raise(ox_parse_error_class, ":size must be a fixnum.\n");
	    }
	    buf_size = NUM2LONG(v);
	}
    }
    b->file = f;
    init(b, fileno(f), indent, buf_size);

    if (rb_block_given_p()) {
	volatile VALUE	rb = Data_Wrap_Struct(builder_class, NULL, builder_free, b);
	rb_yield(rb);
	bclose(b);
	return Qnil;
    } else {
	return Data_Wrap_Struct(builder_class, NULL, builder_free, b);
    }
}

/* call-seq: io(io, options)
 *
 * Creates a new Builder that will write to an IO instance.
 *
 * - +io+ (String) IO to write to
 * - +options+ - (Hash) formating options
 *   - +:indent+ (Fixnum) indentaion level
 *   - +:size+ (Fixnum) the initial size of the string buffer
 */
static VALUE
builder_io(int argc, VALUE *argv, VALUE self) {
    Builder		b = ALLOC(struct _Builder);
    int			indent = ox_default_options.indent;
    long		buf_size = 0;
    int			fd;
    volatile VALUE	v;
    
    if (1 > argc) {
	rb_raise(ox_arg_error_class, "missing IO object");
    }
    if (!rb_respond_to(*argv, ox_fileno_id) ||
	Qnil == (v = rb_funcall(*argv, ox_fileno_id, 0)) ||
	0 == (fd = FIX2INT(v))) {
	rb_raise(rb_eIOError, "expected an IO that has a fileno.");
    }
    if (2 == argc) {
	volatile VALUE	v;

	rb_check_type(argv[1], T_HASH);
	if (Qnil != (v = rb_hash_lookup(argv[1], ox_indent_sym))) {
	    if (rb_cFixnum != rb_obj_class(v)) {
		rb_raise(ox_parse_error_class, ":indent must be a fixnum.\n");
	    }
	    indent = NUM2INT(v);
	}
	if (Qnil != (v = rb_hash_lookup(argv[1], ox_size_sym))) {
	    if (rb_cFixnum != rb_obj_class(v)) {
		rb_raise(ox_parse_error_class, ":size must be a fixnum.\n");
	    }
	    buf_size = NUM2LONG(v);
	}
    }
    b->file = NULL;
    init(b, fd, indent, buf_size);

    if (rb_block_given_p()) {
	volatile VALUE	rb = Data_Wrap_Struct(builder_class, NULL, builder_free, b);
	rb_yield(rb);
	bclose(b);
	return Qnil;
    } else {
	return Data_Wrap_Struct(builder_class, NULL, builder_free, b);
    }
}

/* call-seq: instruct(decl,options)
 *
 * Adds the top level <?xml?> element.
 *
 * - +decl+ - (String) 'xml' expected
 * - +options+ - (Hash) version or encoding
 */
static VALUE
builder_instruct(int argc, VALUE *argv, VALUE self) {
    Builder	b = (Builder)DATA_PTR(self);

    i_am_a_child(b, false);
    append_indent(b);
    if (0 == argc) {
	buf_append_string(&b->buf, "<?xml?>", 7);
    } else {
	volatile VALUE	v;
	
	buf_append_string(&b->buf, "<?", 2);
	append_sym_str(&b->buf, *argv);
	if (1 < argc && rb_cHash == rb_obj_class(argv[1])) {
	    if (Qnil != (v = rb_hash_lookup(argv[1], ox_version_sym))) {
		if (rb_cString != rb_obj_class(v)) {
		    rb_raise(ox_parse_error_class, ":version must be a Symbol.\n");
		}
		buf_append_string(&b->buf, " version=\"", 10);
		buf_append_string(&b->buf, StringValuePtr(v), RSTRING_LEN(v));
		buf_append(&b->buf, '"');
	    }
	    if (Qnil != (v = rb_hash_lookup(argv[1], ox_encoding_sym))) {
		if (rb_cString != rb_obj_class(v)) {
		    rb_raise(ox_parse_error_class, ":encoding must be a Symbol.\n");
		}
		buf_append_string(&b->buf, " encoding=\"", 11);
		buf_append_string(&b->buf, StringValuePtr(v), RSTRING_LEN(v));
		buf_append(&b->buf, '"');
		strncpy(b->encoding, StringValuePtr(v), sizeof(b->encoding));
		b->encoding[sizeof(b->encoding) - 1] = '\0';
	    }
	    if (Qnil != (v = rb_hash_lookup(argv[1], ox_standalone_sym))) {
		if (rb_cString != rb_obj_class(v)) {
		    rb_raise(ox_parse_error_class, ":standalone must be a Symbol.\n");
		}
		buf_append_string(&b->buf, " standalone=\"", 13);
		buf_append_string(&b->buf, StringValuePtr(v), RSTRING_LEN(v));
		buf_append(&b->buf, '"');
	    }
	}
	buf_append_string(&b->buf, "?>", 2);
    }
    return Qnil;
}

/* call-seq: element(name,attributes)
 *
 * Adds an element with the name and attributes provided. If a block is given
 * then on closing of the block a pop() done at the close of the block.
 *
 * - +name+ - (String) name of the element
 * - +attributes+ - (Hash) of the element
 */
static VALUE
builder_element(int argc, VALUE *argv, VALUE self) {
    Builder		b = (Builder)DATA_PTR(self);
    Element		e;
    const char		*name;
    int			len;

    if (1 > argc) {
	rb_raise(ox_arg_error_class, "missing element name");
    }
    i_am_a_child(b, false);
    append_indent(b);
    b->depth++;
    if (MAX_DEPTH <= b->depth) {
	rb_raise(ox_arg_error_class, "XML too deeply nested");
    }
    switch (rb_type(*argv)) {
    case T_STRING:
	name = StringValuePtr(*argv);
	len = RSTRING_LEN(*argv);
	break;
    case T_SYMBOL:
	name = rb_id2name(SYM2ID(*argv));
	len = strlen(name);
	break;
    default:
	rb_raise(ox_arg_error_class, "expected a Symbol or String for an element name");
	break;
    }
    e = &b->stack[b->depth];
    if (sizeof(e->buf) <= len) {
	e->name = strdup(name);
	*e->buf = '\0';
    } else {
	strcpy(e->buf, name);
	e->name = e->buf;
    }
    e->len = len;
    e->has_child = false;
    e->non_text_child = false;

    buf_append(&b->buf, '<');
    buf_append_string(&b->buf, e->name, len);
    if (1 < argc) {
	rb_hash_foreach(argv[1], append_attr, (VALUE)b);
    }
    // Do not close with > or /> yet. That is done with i_am_a_child() or pop().
    if (rb_block_given_p()) {
	rb_yield(self);
	pop(b);
    }
    return Qnil;
}

/* call-seq: comment(text)
 *
 * Adds a comment element to the XML string being formed.
 * - +text+ - (String) contents of the comment
 */
static VALUE
builder_comment(VALUE self, VALUE text) {
    Builder	b = (Builder)DATA_PTR(self);

    i_am_a_child(b, false);
    append_indent(b);
    buf_append_string(&b->buf, "<!-- ", 5);
    buf_append_string(&b->buf, StringValuePtr(text), RSTRING_LEN(text));
    buf_append_string(&b->buf, " --/> ", 5);

    return Qnil;
}

/* call-seq: doctype(text)
 *
 * Adds a DOCTYPE element to the XML string being formed.
 * - +text+ - (String) contents of the doctype
 */
static VALUE
builder_doctype(VALUE self, VALUE text) {
    Builder	b = (Builder)DATA_PTR(self);

    i_am_a_child(b, false);
    append_indent(b);
    buf_append_string(&b->buf, "<!DOCTYPE ", 10);
    buf_append_string(&b->buf, StringValuePtr(text), RSTRING_LEN(text));
    buf_append(&b->buf, '>');

    return Qnil;
}

/* call-seq: text(text)
 *
 * Adds a text element to the XML string being formed.
 * - +text+ - (String) contents of the text field
 */
static VALUE
builder_text(VALUE self, VALUE text) {
    Builder	b = (Builder)DATA_PTR(self);

    i_am_a_child(b, true);
    append_string(&b->buf, StringValuePtr(text), RSTRING_LEN(text));

    return Qnil;
}

/* call-seq: cdata(data)
 *
 * Adds a CDATA element to the XML string being formed.
 * - +data+ - (String) contents of the CDATA element
 */
static VALUE
builder_cdata(VALUE self, VALUE data) {
    Builder	b = (Builder)DATA_PTR(self);

    i_am_a_child(b, false);
    append_indent(b);
    buf_append_string(&b->buf, "<![CDATA[", 9);
    buf_append_string(&b->buf, StringValuePtr(data), RSTRING_LEN(data));
    buf_append_string(&b->buf, "]]>", 3);

    return Qnil;
}

/* call-seq: raw(text)
 *
 * Adds the provided string directly to the XML without formatting or modifications.
 *
 * - +text+ - (String) contents to be added
 */
static VALUE
builder_raw(VALUE self, VALUE text) {
    Builder	b = (Builder)DATA_PTR(self);

    i_am_a_child(b, true);
    buf_append_string(&b->buf, StringValuePtr(text), RSTRING_LEN(text));

    return Qnil;
}

/* call-seq: to_s()
 *
 * Returns the JSON document string in what ever state the construction is at.
 */
static VALUE
builder_to_s(VALUE self) {
    return to_s((Builder)DATA_PTR(self));
}

/* call-seq: pop()
 *
 * Closes the current element.
 */
static VALUE
builder_pop(VALUE self) {
    pop((Builder)DATA_PTR(self));

    return Qnil;
}

/* call-seq: close()
 *
 * Closes the all elements and the document.
 */
static VALUE
builder_close(VALUE self) {
    bclose((Builder)DATA_PTR(self));

    return Qnil;
}

/*
 * Document-class: Ox::Builder
 *
 * An XML builder.
 */
void ox_init_builder(VALUE ox) {
#if 0
    ox = rb_define_module("Ox");
#endif
    builder_class = rb_define_class_under(ox, "Builder", rb_cObject);
    rb_define_module_function(builder_class, "new", builder_new, -1);
    rb_define_module_function(builder_class, "file", builder_file, -1);
    rb_define_module_function(builder_class, "io", builder_io, -1);
    rb_define_method(builder_class, "instruct", builder_instruct, -1);
    rb_define_method(builder_class, "comment", builder_comment, 1);
    rb_define_method(builder_class, "doctype", builder_doctype, 1);
    rb_define_method(builder_class, "element", builder_element, -1);
    rb_define_method(builder_class, "text", builder_text, 1);
    rb_define_method(builder_class, "cdata", builder_cdata, 1);
    rb_define_method(builder_class, "raw", builder_raw, 1);
    rb_define_method(builder_class, "pop", builder_pop, 0);
    rb_define_method(builder_class, "close", builder_close, 0);
    rb_define_method(builder_class, "to_s", builder_to_s, 0);
}