
#include "aidl_language.h"
#include "options.h"
#include "os.h"
#include "search_path.h"
#include "Type.h"
#include "generate_java.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>

#ifdef _WIN32
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#endif

#ifndef O_BINARY
#  define O_BINARY  0
#endif

// The following are gotten as the offset from the allowable id's between
// android.os.IBinder.FIRST_CALL_TRANSACTION=1 and
// android.os.IBinder.LAST_CALL_TRANSACTION=16777215
#define MIN_USER_SET_METHOD_ID                0
#define MAX_USER_SET_METHOD_ID                16777214

namespace android {
namespace aidl {

using std::map;
using std::set;
using std::string;
using std::vector;

static void
test_document(document_item_type* d)
{
    while (d) {
        if (d->item_type == INTERFACE_TYPE_BINDER) {
            interface_type* c = (interface_type*)d;
            printf("interface %s %s {\n", c->package, c->name.data);
            interface_item_type *q = (interface_item_type*)c->interface_items;
            while (q) {
                if (q->item_type == METHOD_TYPE) {
                    method_type *m = (method_type*)q;
                    printf("  %s %s(", m->type.type.data, m->name.data);
                    arg_type *p = m->args;
                    while (p) {
                        printf("%s %s",p->type.type.data,p->name.data);
                        if (p->next) printf(", ");
                        p=p->next;
                    }
                    printf(")");
                    printf(";\n");
                }
                q=q->next;
            }
            printf("}\n");
        }
        else if (d->item_type == USER_DATA_TYPE) {
            user_data_type* b = (user_data_type*)d;
            if (b->parcelable) {
                printf("parcelable %s %s;\n", b->package, b->name.data);
            }
        }
        else {
            printf("UNKNOWN d=0x%08lx d->item_type=%d\n", (long)d, d->item_type);
        }
        d = d->next;
    }
}

// ==========================================================
int
convert_direction(const char* direction)
{
    if (direction == NULL) {
        return IN_PARAMETER;
    }
    if (0 == strcmp(direction, "in")) {
        return IN_PARAMETER;
    }
    if (0 == strcmp(direction, "out")) {
        return OUT_PARAMETER;
    }
    return INOUT_PARAMETER;
}

// ==========================================================
struct import_info {
    const char* from;
    const char* filename;
    buffer_type statement;
    const char* neededClass;
    document_item_type* doc;
    struct import_info* next;
};

document_item_type* g_document = NULL;
import_info* g_imports = NULL;

static void
main_document_parsed(document_item_type* d)
{
    g_document = d;
}

static void
main_import_parsed(buffer_type* statement)
{
    import_info* import = (import_info*)malloc(sizeof(import_info));
    memset(import, 0, sizeof(import_info));
    import->from = strdup(psGlobal->FileName().c_str());
    import->statement.lineno = statement->lineno;
    import->statement.data = strdup(statement->data);
    import->statement.extra = NULL;
    import->next = g_imports;
    import->neededClass = parse_import_statement(statement->data);
    g_imports = import;
}

static ParserCallbacks g_mainCallbacks = {
    &main_document_parsed,
    &main_import_parsed
};

char*
parse_import_statement(const char* text)
{
    const char* end;
    int len;

    while (isspace(*text)) {
        text++;
    }
    while (!isspace(*text)) {
        text++;
    }
    while (isspace(*text)) {
        text++;
    }
    end = text;
    while (!isspace(*end) && *end != ';') {
        end++;
    }
    len = end-text;

    char* rv = (char*)malloc(len+1);
    memcpy(rv, text, len);
    rv[len] = '\0';

    return rv;
}

// ==========================================================
static void
import_import_parsed(buffer_type* statement)
{
}

// ==========================================================
static int
check_filename(const char* filename, const char* package, buffer_type* name)
{
    const char* p;
    string expected;
    string fn;
    size_t len;
    char cwd[MAXPATHLEN];
    bool valid = false;

#ifdef _WIN32
    if (isalpha(filename[0]) && filename[1] == ':'
        && filename[2] == OS_PATH_SEPARATOR) {
#else
    if (filename[0] == OS_PATH_SEPARATOR) {
#endif
        fn = filename;
    } else {
        fn = getcwd(cwd, sizeof(cwd));
        len = fn.length();
        if (fn[len-1] != OS_PATH_SEPARATOR) {
            fn += OS_PATH_SEPARATOR;
        }
        fn += filename;
    }

    if (package) {
        expected = package;
        expected += '.';
    }

    len = expected.length();
    for (size_t i=0; i<len; i++) {
        if (expected[i] == '.') {
            expected[i] = OS_PATH_SEPARATOR;
        }
    }

    p = strchr(name->data, '.');
    len = p ? p-name->data : strlen(name->data);
    expected.append(name->data, len);

    expected += ".aidl";

    len = fn.length();
    valid = (len >= expected.length());

    if (valid) {
        p = fn.c_str() + (len - expected.length());

#ifdef _WIN32
        if (OS_PATH_SEPARATOR != '/') {
            // Input filename under cygwin most likely has / separators
            // whereas the expected string uses \\ separators. Adjust
            // them accordingly.
          for (char *c = const_cast<char *>(p); *c; ++c) {
                if (*c == '/') *c = OS_PATH_SEPARATOR;
            }
        }
#endif

        // aidl assumes case-insensitivity on Mac Os and Windows.
#if defined(__linux__)
        valid = (expected == p);
#else
        valid = !strcasecmp(expected.c_str(), p);
#endif
    }

    if (!valid) {
        fprintf(stderr, "%s:%d interface %s should be declared in a file"
                " called %s.\n",
                filename, name->lineno, name->data, expected.c_str());
        return 1;
    }

    return 0;
}

static int
check_filenames(const char* filename, document_item_type* items)
{
    int err = 0;
    while (items) {
        if (items->item_type == USER_DATA_TYPE) {
            user_data_type* p = (user_data_type*)items;
            err |= check_filename(filename, p->package, &p->name);
        }
        else if (items->item_type == INTERFACE_TYPE_BINDER) {
            interface_type* c = (interface_type*)items;
            err |= check_filename(filename, c->package, &c->name);
        }
        else {
            fprintf(stderr, "aidl: internal error unkown document type %d.\n",
                        items->item_type);
            return 1;
        }
        items = items->next;
    }
    return err;
}

// ==========================================================
static const char*
kind_to_string(int kind)
{
    switch (kind)
    {
        case Type::INTERFACE:
            return "an interface";
        case Type::USERDATA:
            return "a user data";
        default:
            return "ERROR";
    }
}

static char*
rfind(char* str, char c)
{
    char* p = str + strlen(str) - 1;
    while (p >= str) {
        if (*p == c) {
            return p;
        }
        p--;
    }
    return NULL;
}

static int
gather_types(const char* filename, document_item_type* items)
{
    int err = 0;
    while (items) {
        Type* type;
        if (items->item_type == USER_DATA_TYPE) {
            user_data_type* p = (user_data_type*)items;
            type = new UserDataType(p->package ? p->package : "", p->name.data,
                    false, p->parcelable, filename, p->name.lineno);
        }
        else if (items->item_type == INTERFACE_TYPE_BINDER) {
            interface_type* c = (interface_type*)items;
            type = new InterfaceType(c->package ? c->package : "",
                            c->name.data, false, c->oneway,
                            filename, c->name.lineno);
        }
        else {
            fprintf(stderr, "aidl: internal error %s:%d\n", __FILE__, __LINE__);
            return 1;
        }

        Type* old = NAMES.Find(type->QualifiedName());
        if (old == NULL) {
            NAMES.Add(type);

            if (items->item_type == INTERFACE_TYPE_BINDER) {
                // for interfaces, also add the stub and proxy types, we don't
                // bother checking these for duplicates, because the parser
                // won't let us do it.
                interface_type* c = (interface_type*)items;

                string name = c->name.data;
                name += ".Stub";
                Type* stub = new Type(c->package ? c->package : "",
                                        name, Type::GENERATED, false, false,
                                        filename, c->name.lineno);
                NAMES.Add(stub);

                name = c->name.data;
                name += ".Stub.Proxy";
                Type* proxy = new Type(c->package ? c->package : "",
                                        name, Type::GENERATED, false, false,
                                        filename, c->name.lineno);
                NAMES.Add(proxy);
            }
        } else {
            if (old->Kind() == Type::BUILT_IN) {
                fprintf(stderr, "%s:%d attempt to redefine built in class %s\n",
                            filename, type->DeclLine(),
                            type->QualifiedName().c_str());
                err = 1;
            }
            else if (type->Kind() != old->Kind()) {
                const char* oldKind = kind_to_string(old->Kind());
                const char* newKind = kind_to_string(type->Kind());

                fprintf(stderr, "%s:%d attempt to redefine %s as %s,\n",
                            filename, type->DeclLine(),
                            type->QualifiedName().c_str(), newKind);
                fprintf(stderr, "%s:%d    previously defined here as %s.\n",
                            old->DeclFile().c_str(), old->DeclLine(), oldKind);
                err = 1;
            }
        }

        items = items->next;
    }
    return err;
}

// ==========================================================
static bool
matches_keyword(const char* str)
{
    static const char* KEYWORDS[] = { "abstract", "assert", "boolean", "break",
        "byte", "case", "catch", "char", "class", "const", "continue",
        "default", "do", "double", "else", "enum", "extends", "final",
        "finally", "float", "for", "goto", "if", "implements", "import",
        "instanceof", "int", "interface", "long", "native", "new", "package",
        "private", "protected", "public", "return", "short", "static",
        "strictfp", "super", "switch", "synchronized", "this", "throw",
        "throws", "transient", "try", "void", "volatile", "while",
        "true", "false", "null",
        NULL
    };
    const char** k = KEYWORDS;
    while (*k) {
        if (0 == strcmp(str, *k)) {
            return true;
        }
        k++;
    }
    return false;
}

static int
check_method(const char* filename, method_type* m)
{
    int err = 0;

    // return type
    Type* returnType = NAMES.Search(m->type.type.data);
    if (returnType == NULL) {
        fprintf(stderr, "%s:%d unknown return type %s\n", filename,
                    m->type.type.lineno, m->type.type.data);
        err = 1;
        return err;
    }

    if (!returnType->CanWriteToParcel()) {
        fprintf(stderr, "%s:%d return type %s can't be marshalled.\n", filename,
                        m->type.type.lineno, m->type.type.data);
        err = 1;
    }

    if (m->type.dimension > 0 && !returnType->CanBeArray()) {
        fprintf(stderr, "%s:%d return type %s%s can't be an array.\n", filename,
                m->type.array_token.lineno, m->type.type.data,
                m->type.array_token.data);
        err = 1;
    }

    if (m->type.dimension > 1) {
        fprintf(stderr, "%s:%d return type %s%s only one"
                " dimensional arrays are supported\n", filename,
                m->type.array_token.lineno, m->type.type.data,
                m->type.array_token.data);
        err = 1;
    }

    int index = 1;

    arg_type* arg = m->args;
    while (arg) {
        Type* t = NAMES.Search(arg->type.type.data);

        // check the arg type
        if (t == NULL) {
            fprintf(stderr, "%s:%d parameter %s (%d) unknown type %s\n",
                    filename, m->type.type.lineno, arg->name.data, index,
                    arg->type.type.data);
            err = 1;
            goto next;
        }

        if (!t->CanWriteToParcel()) {
            fprintf(stderr, "%s:%d parameter %d: '%s %s' can't be marshalled.\n",
                        filename, m->type.type.lineno, index,
                        arg->type.type.data, arg->name.data);
            err = 1;
        }

        if (arg->direction.data == NULL
                && (arg->type.dimension != 0 || t->CanBeOutParameter())) {
            fprintf(stderr, "%s:%d parameter %d: '%s %s' can be an out"
                                " parameter, so you must declare it as in,"
                                " out or inout.\n",
                        filename, m->type.type.lineno, index,
                        arg->type.type.data, arg->name.data);
            err = 1;
        }

        if (convert_direction(arg->direction.data) != IN_PARAMETER
                && !t->CanBeOutParameter()
                && arg->type.dimension == 0) {
            fprintf(stderr, "%s:%d parameter %d: '%s %s %s' can only be an in"
                            " parameter.\n",
                        filename, m->type.type.lineno, index,
                        arg->direction.data, arg->type.type.data,
                        arg->name.data);
            err = 1;
        }

        if (arg->type.dimension > 0 && !t->CanBeArray()) {
            fprintf(stderr, "%s:%d parameter %d: '%s %s%s %s' can't be an"
                    " array.\n", filename,
                    m->type.array_token.lineno, index, arg->direction.data,
                    arg->type.type.data, arg->type.array_token.data,
                    arg->name.data);
            err = 1;
        }

        if (arg->type.dimension > 1) {
            fprintf(stderr, "%s:%d parameter %d: '%s %s%s %s' only one"
                    " dimensional arrays are supported\n", filename,
                    m->type.array_token.lineno, index, arg->direction.data,
                    arg->type.type.data, arg->type.array_token.data,
                    arg->name.data);
            err = 1;
        }

        // check that the name doesn't match a keyword
        if (matches_keyword(arg->name.data)) {
            fprintf(stderr, "%s:%d parameter %d %s is named the same as a"
                    " Java or aidl keyword\n",
                    filename, m->name.lineno, index, arg->name.data);
            err = 1;
        }

next:
        index++;
        arg = arg->next;
    }

    return err;
}

static int
check_types(const char* filename, document_item_type* items)
{
    int err = 0;
    while (items) {
        // (nothing to check for USER_DATA_TYPE)
        if (items->item_type == INTERFACE_TYPE_BINDER) {
            map<string,method_type*> methodNames;
            interface_type* c = (interface_type*)items;

            interface_item_type* member = c->interface_items;
            while (member) {
                if (member->item_type == METHOD_TYPE) {
                    method_type* m = (method_type*)member;

                    err |= check_method(filename, m);

                    // prevent duplicate methods
                    if (methodNames.find(m->name.data) == methodNames.end()) {
                        methodNames[m->name.data] = m;
                    } else {
                        fprintf(stderr,"%s:%d attempt to redefine method %s,\n",
                                filename, m->name.lineno, m->name.data);
                        method_type* old = methodNames[m->name.data];
                        fprintf(stderr, "%s:%d    previously defined here.\n",
                                filename, old->name.lineno);
                        err = 1;
                    }
                }
                member = member->next;
            }
        }

        items = items->next;
    }
    return err;
}

// ==========================================================
static int
exactly_one_interface(const char* filename, const document_item_type* items, const Options& options,
                      bool* onlyParcelable)
{
    if (items == NULL) {
        fprintf(stderr, "%s: file does not contain any interfaces\n",
                            filename);
        return 1;
    }

    const document_item_type* next = items->next;
    // Allow parcelables to skip the "one-only" rule.
    if (items->next != NULL && next->item_type != USER_DATA_TYPE) {
        int lineno = -1;
        if (next->item_type == INTERFACE_TYPE_BINDER) {
            lineno = ((interface_type*)next)->interface_token.lineno;
        }
        fprintf(stderr, "%s:%d aidl can only handle one interface per file\n",
                            filename, lineno);
        return 1;
    }

    if (items->item_type == USER_DATA_TYPE) {
        *onlyParcelable = true;
        if (options.fail_on_parcelable_) {
            fprintf(stderr, "%s:%d aidl can only generate code for interfaces, not"
                            " parcelables,\n", filename,
                            ((user_data_type*)items)->keyword_token.lineno);
            fprintf(stderr, "%s:%d .aidl files that only declare parcelables"
                            "may not go in the Makefile.\n", filename,
                            ((user_data_type*)items)->keyword_token.lineno);
            return 1;
        }
    } else {
        *onlyParcelable = false;
    }

    return 0;
}

// ==========================================================
void
generate_dep_file(const Options& options, const document_item_type* items)
{
    /* we open the file in binary mode to ensure that the same output is
     * generated on all platforms !!
     */
    FILE* to = NULL;
    if (options.auto_dep_file_) {
        string fileName = options.output_file_name_ + ".d";
        to = fopen(fileName.c_str(), "wb");
    } else {
        to = fopen(options.dep_file_name_.c_str(), "wb");
    }

    if (to == NULL) {
        return;
    }

    const char* slash = "\\";
    import_info* import = g_imports;
    if (import == NULL) {
        slash = "";
    }

    if (items->item_type == INTERFACE_TYPE_BINDER) {
        fprintf(to, "%s: \\\n", options.output_file_name_.c_str());
    } else {
        // parcelable: there's no output file.
        fprintf(to, " : \\\n");
    }
    fprintf(to, "  %s %s\n", options.input_file_name_.c_str(), slash);

    while (import) {
        if (import->next == NULL) {
            slash = "";
        }
        if (import->filename) {
            fprintf(to, "  %s %s\n", import->filename, slash);
        }
        import = import->next;
    }

    fprintf(to, "\n");

    // Output "<input_aidl_file>: " so make won't fail if the input .aidl file
    // has been deleted, moved or renamed in incremental build.
    fprintf(to, "%s :\n", options.input_file_name_.c_str());

    // Output "<imported_file>: " so make won't fail if the imported file has
    // been deleted, moved or renamed in incremental build.
    import = g_imports;
    while (import) {
        if (import->filename) {
            fprintf(to, "%s :\n", import->filename);
        }
        import = import->next;
    }

    fclose(to);
}

// ==========================================================
static string
generate_outputFileName2(const Options& options, const buffer_type& name, const char* package)
{
    string result;

    // create the path to the destination folder based on the
    // interface package name
    result = options.output_base_folder_;
    result += OS_PATH_SEPARATOR;

    string packageStr = package;
    size_t len = packageStr.length();
    for (size_t i=0; i<len; i++) {
        if (packageStr[i] == '.') {
            packageStr[i] = OS_PATH_SEPARATOR;
        }
    }

    result += packageStr;

    // add the filename by replacing the .aidl extension to .java
    const char* p = strchr(name.data, '.');
    len = p ? p-name.data : strlen(name.data);

    result += OS_PATH_SEPARATOR;
    result.append(name.data, len);
    result += ".java";

    return result;
}

// ==========================================================
static string
generate_outputFileName(const Options& options, const document_item_type* items)
{
    // items has already been checked to have only one interface.
    if (items->item_type == INTERFACE_TYPE_BINDER) {
        interface_type* type = (interface_type*)items;

        return generate_outputFileName2(options, type->name, type->package);
    } else if (items->item_type == USER_DATA_TYPE) {
        user_data_type* type = (user_data_type*)items;
        return generate_outputFileName2(options, type->name, type->package);
    }

    // I don't think we can come here, but safer than returning NULL.
    string result;
    return result;
}



// ==========================================================
static void
check_outputFilePath(const string& path) {
    size_t len = path.length();
    for (size_t i=0; i<len ; i++) {
        if (path[i] == OS_PATH_SEPARATOR) {
            string p = path.substr(0, i);
            if (access(path.data(), F_OK) != 0) {
#ifdef _WIN32
                _mkdir(p.data());
#else
                mkdir(p.data(), S_IRUSR|S_IWUSR|S_IXUSR|S_IRGRP|S_IXGRP);
#endif
            }
        }
    }
}


// ==========================================================
static int
parse_preprocessed_file(const string& filename)
{
    int err;

    FILE* f = fopen(filename.c_str(), "rb");
    if (f == NULL) {
        fprintf(stderr, "aidl: can't open preprocessed file: %s\n",
                filename.c_str());
        return 1;
    }

    int lineno = 1;
    char line[1024];
    char type[1024];
    char fullname[1024];
    while (fgets(line, sizeof(line), f)) {
        // skip comments and empty lines
        if (!line[0] || strncmp(line, "//", 2) == 0) {
          continue;
        }

        sscanf(line, "%s %[^; \r\n\t];", type, fullname);

        char* packagename;
        char* classname = rfind(fullname, '.');
        if (classname != NULL) {
            *classname = '\0';
            classname++;
            packagename = fullname;
        } else {
            classname = fullname;
            packagename = NULL;
        }

        //printf("%s:%d:...%s...%s...%s...\n", filename.c_str(), lineno,
        //        type, packagename, classname);
        document_item_type* doc;

        if (0 == strcmp("parcelable", type)) {
            user_data_type* parcl = (user_data_type*)malloc(
                    sizeof(user_data_type));
            memset(parcl, 0, sizeof(user_data_type));
            parcl->document_item.item_type = USER_DATA_TYPE;
            parcl->keyword_token.lineno = lineno;
            parcl->keyword_token.data = strdup(type);
            parcl->package = packagename ? strdup(packagename) : NULL;
            parcl->name.lineno = lineno;
            parcl->name.data = strdup(classname);
            parcl->semicolon_token.lineno = lineno;
            parcl->semicolon_token.data = strdup(";");
            parcl->parcelable = true;
            doc = (document_item_type*)parcl;
        }
        else if (0 == strcmp("interface", type)) {
            interface_type* iface = (interface_type*)malloc(
                    sizeof(interface_type));
            memset(iface, 0, sizeof(interface_type));
            iface->document_item.item_type = INTERFACE_TYPE_BINDER;
            iface->interface_token.lineno = lineno;
            iface->interface_token.data = strdup(type);
            iface->package = packagename ? strdup(packagename) : NULL;
            iface->name.lineno = lineno;
            iface->name.data = strdup(classname);
            iface->open_brace_token.lineno = lineno;
            iface->open_brace_token.data = strdup("{");
            iface->close_brace_token.lineno = lineno;
            iface->close_brace_token.data = strdup("}");
            doc = (document_item_type*)iface;
        }
        else {
            fprintf(stderr, "%s:%d: bad type in line: %s\n",
                    filename.c_str(), lineno, line);
            fclose(f);
            return 1;
        }
        err = gather_types(filename.c_str(), doc);
        lineno++;
    }

    if (!feof(f)) {
        fprintf(stderr, "%s:%d: error reading file, line to long.\n",
                filename.c_str(), lineno);
        return 1;
    }

    fclose(f);
    return 0;
}

static int
check_and_assign_method_ids(const char * filename, interface_item_type* first_item)
{
    // Check whether there are any methods with manually assigned id's and any that are not.
    // Either all method id's must be manually assigned or all of them must not.
    // Also, check for duplicates of user set id's and that the id's are within the proper bounds.
    set<int> usedIds;
    interface_item_type* item = first_item;
    bool hasUnassignedIds = false;
    bool hasAssignedIds = false;
    while (item != NULL) {
        if (item->item_type == METHOD_TYPE) {
            method_type* method_item = (method_type*)item;
            if (method_item->hasId) {
                hasAssignedIds = true;
                method_item->assigned_id = atoi(method_item->id.data);
                // Ensure that the user set id is not duplicated.
                if (usedIds.find(method_item->assigned_id) != usedIds.end()) {
                    // We found a duplicate id, so throw an error.
                    fprintf(stderr,
                            "%s:%d Found duplicate method id (%d) for method: %s\n",
                            filename, method_item->id.lineno,
                            method_item->assigned_id, method_item->name.data);
                    return 1;
                }
                // Ensure that the user set id is within the appropriate limits
                if (method_item->assigned_id < MIN_USER_SET_METHOD_ID ||
                        method_item->assigned_id > MAX_USER_SET_METHOD_ID) {
                    fprintf(stderr, "%s:%d Found out of bounds id (%d) for method: %s\n",
                            filename, method_item->id.lineno,
                            method_item->assigned_id, method_item->name.data);
                    fprintf(stderr, "    Value for id must be between %d and %d inclusive.\n",
                            MIN_USER_SET_METHOD_ID, MAX_USER_SET_METHOD_ID);
                    return 1;
                }
                usedIds.insert(method_item->assigned_id);
            } else {
                hasUnassignedIds = true;
            }
            if (hasAssignedIds && hasUnassignedIds) {
                fprintf(stderr,
                        "%s: You must either assign id's to all methods or to none of them.\n",
                        filename);
                return 1;
            }
        }
        item = item->next;
    }

    // In the case that all methods have unassigned id's, set a unique id for them.
    if (hasUnassignedIds) {
        int newId = 0;
        item = first_item;
        while (item != NULL) {
            if (item->item_type == METHOD_TYPE) {
                method_type* method_item = (method_type*)item;
                method_item->assigned_id = newId++;
            }
            item = item->next;
        }
    }

    // success
    return 0;
}

// ==========================================================
int
compile_aidl(const Options& options)
{
    int err = 0, N;

    set_import_paths(options.import_paths_);

    register_base_types();

    // import the preprocessed file
    N = options.preprocessed_files_.size();
    for (int i=0; i<N; i++) {
        const string& s = options.preprocessed_files_[i];
        err |= parse_preprocessed_file(s);
    }
    if (err != 0) {
        return err;
    }

    // parse the main file
    g_callbacks = &g_mainCallbacks;
    err = parse_aidl(options.input_file_name_.c_str());
    document_item_type* mainDoc = g_document;
    g_document = NULL;

    // parse the imports
    g_callbacks = &g_mainCallbacks;
    import_info* import = g_imports;
    while (import) {
        if (NAMES.Find(import->neededClass) == NULL) {
            import->filename = find_import_file(import->neededClass);
            if (!import->filename) {
                fprintf(stderr, "%s:%d: couldn't find import for class %s\n",
                        import->from, import->statement.lineno,
                        import->neededClass);
                err |= 1;
            } else {
                err |= parse_aidl(import->filename);
                import->doc = g_document;
                if (import->doc == NULL) {
                    err |= 1;
                }
            }
        }
        import = import->next;
    }
    // bail out now if parsing wasn't successful
    if (err != 0 || mainDoc == NULL) {
        //fprintf(stderr, "aidl: parsing failed, stopping.\n");
        return 1;
    }

    // complain about ones that aren't in the right files
    err |= check_filenames(options.input_file_name_.c_str(), mainDoc);
    import = g_imports;
    while (import) {
        err |= check_filenames(import->filename, import->doc);
        import = import->next;
    }

    // gather the types that have been declared
    err |= gather_types(options.input_file_name_.c_str(), mainDoc);
    import = g_imports;
    while (import) {
        err |= gather_types(import->filename, import->doc);
        import = import->next;
    }

#if 0
    printf("---- main doc ----\n");
    test_document(mainDoc);

    import = g_imports;
    while (import) {
        printf("---- import doc ----\n");
        test_document(import->doc);
        import = import->next;
    }
    NAMES.Dump();
#endif

    // check the referenced types in mainDoc to make sure we've imported them
    err |= check_types(options.input_file_name_.c_str(), mainDoc);

    // finally, there really only needs to be one thing in mainDoc, and it
    // needs to be an interface.
    bool onlyParcelable = false;
    err |= exactly_one_interface(options.input_file_name_.c_str(), mainDoc, options, &onlyParcelable);

    // If this includes an interface definition, then assign method ids and validate.
    if (!onlyParcelable) {
        err |= check_and_assign_method_ids(options.input_file_name_.c_str(),
                ((interface_type*)mainDoc)->interface_items);
    }

    // after this, there shouldn't be any more errors because of the
    // input.
    if (err != 0 || mainDoc == NULL) {
        return 1;
    }

    string output_file_name = options.output_file_name_;
    // if needed, generate the output file name from the base folder
    if (output_file_name.length() == 0 && options.output_base_folder_.length() > 0) {
        output_file_name = generate_outputFileName(options, mainDoc);
    }

    // if we were asked to, generate a make dependency file
    // unless it's a parcelable *and* it's supposed to fail on parcelable
    if ((options.auto_dep_file_ || options.dep_file_name_ != "") &&
            !(onlyParcelable && options.fail_on_parcelable_)) {
        // make sure the folders of the output file all exists
        check_outputFilePath(output_file_name);
        generate_dep_file(options, mainDoc);
    }

    // they didn't ask to fail on parcelables, so just exit quietly.
    if (onlyParcelable && !options.fail_on_parcelable_) {
        return 0;
    }

    // make sure the folders of the output file all exists
    check_outputFilePath(output_file_name);

    err = generate_java(output_file_name, options.input_file_name_.c_str(),
                        (interface_type*)mainDoc);

    return err;
}

int
preprocess_aidl(const Options& options)
{
    vector<string> lines;
    int err;

    // read files
    int N = options.files_to_preprocess_.size();
    for (int i=0; i<N; i++) {
        g_callbacks = &g_mainCallbacks;
        err = parse_aidl(options.files_to_preprocess_[i].c_str());
        if (err != 0) {
            return err;
        }
        document_item_type* doc = g_document;
        string line;
        if (doc->item_type == USER_DATA_TYPE) {
            user_data_type* parcelable = (user_data_type*)doc;
            if (parcelable->parcelable) {
                line = "parcelable ";
            }
            if (parcelable->package) {
                line += parcelable->package;
                line += '.';
            }
            line += parcelable->name.data;
        } else {
            line = "interface ";
            interface_type* iface = (interface_type*)doc;
            if (iface->package) {
                line += iface->package;
                line += '.';
            }
            line += iface->name.data;
        }
        line += ";\n";
        lines.push_back(line);
    }

    // write preprocessed file
    int fd = open( options.output_file_name_.c_str(),
                   O_RDWR|O_CREAT|O_TRUNC|O_BINARY,
#ifdef _WIN32
                   _S_IREAD|_S_IWRITE);
#else
                   S_IRUSR|S_IWUSR|S_IRGRP);
#endif
    if (fd == -1) {
        fprintf(stderr, "aidl: could not open file for write: %s\n",
                options.output_file_name_.c_str());
        return 1;
    }

    N = lines.size();
    for (int i=0; i<N; i++) {
        const string& s = lines[i];
        int len = s.length();
        if (len != write(fd, s.c_str(), len)) {
            fprintf(stderr, "aidl: error writing to file %s\n",
                options.output_file_name_.c_str());
            close(fd);
            unlink(options.output_file_name_.c_str());
            return 1;
        }
    }

    close(fd);
    return 0;
}

}  // namespace android
}  // namespace aidl