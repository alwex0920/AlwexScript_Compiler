#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#ifdef _WIN32
    #include <windows.h>
    #define PATH_SEP '\\'
#else
    #include <unistd.h>
    #define PATH_SEP '/'
#endif

#define MAX_LINE_LEN 1024
#define MAX_CODE_LEN (1024*1024)

static int in_class = 0;   // флаг, что мы внутри тела класса
#define MAX_IF_DEPTH 100
static int if_depth = 0;
static int if_need_close[MAX_IF_DEPTH] = {0};
static int if_base_current_indent[MAX_IF_DEPTH] = {0};

static int current_indent = 0;   // глобальный текущий отступ

// ---------- служебные функции (нужны компилятору) ----------
static int my_isspace(int c) {
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r');
}

double str_to_double(const char* s) {
    double res = 0.0;
    double fact = 1.0;
    int point_seen = 0;
    int negative = 0;
    if (*s == '-') {
        negative = 1;
        s++;
    }
    for (; *s; s++) {
        if (*s == '.') {
            point_seen = 1;
            continue;
        }
        int d = *s - '0';
        if (d >= 0 && d <= 9) {
            if (point_seen) {
                fact /= 10.0;
                res = res + (double)d * fact;
            } else {
                res = res * 10.0 + (double)d;
            }
        } else {
            break;
        }
    }
    return negative ? -res : res;
}

// ---------- утилиты ----------
void str_replace_char(char* str, char from, char to) {
    for (; *str; str++) if (*str == from) *str = to;
}

void escape_for_c_string(const char* src, char* dst, size_t dst_size) {
    // наивное экранирование: \ -> \\, " -> \", новая строка убирается
    size_t j = 0;
    for (const char* p = src; *p && j < dst_size-2; p++) {
        switch (*p) {
            case '\\': dst[j++] = '\\'; dst[j++] = '\\'; break;
            case '"':  dst[j++] = '\\'; dst[j++] = '"'; break;
            case '\n': dst[j++] = '\\'; dst[j++] = 'n'; break;
            case '\r': dst[j++] = '\\'; dst[j++] = 'r'; break;
            default: dst[j++] = *p;
        }
    }
    dst[j] = '\0';
}

// ---------- генерация C кода ----------
void compile_line(const char* token, FILE* out, const char** pp);

void compile_block(const char** pp, FILE* out, int is_func_body) {
    const char* p = *pp;
    while (*p) {
        char line[MAX_LINE_LEN];
        int i = 0;
        while (*p && *p != '\n' && i < MAX_LINE_LEN-1) line[i++] = *p++;
        line[i] = '\0';
        if (*p == '\n') p++;

        // выход для тела функции по end
        if (is_func_body && strncmp(line, "end", 3) == 0 && strncmp(line, "endloop", 7) != 0) {
            *pp = p;
            return;
        }
        // выход для цикла while по endloop
        if (!is_func_body && strncmp(line, "endloop", 7) == 0) {
            *pp = p;
            return;
        }

        compile_line(line, out, &p);
    }
    *pp = p;
}

// ------------------------------------------------------------
// Извлекает все функции из кода, выводит их определения в out,
// и возвращает строку с оставшимся кодом (без func...end).
// Память для возвращаемой строки выделяется через malloc,
// её нужно освободить вызывающей стороне.
// ------------------------------------------------------------
char* extract_and_compile_functions(const char* code, FILE* out) {
    size_t code_len = strlen(code);
    char* clean_code = malloc(code_len + 1);
    if (!clean_code) return NULL;
    size_t clean_pos = 0;

    const char* p = code;
    while (*p) {
        // Ищем начало функции: "func " в начале строки (возможно с пробелами)
        const char* line_start = p;
        // пропускаем пробелы и табуляции
        while (*line_start == ' ' || *line_start == '\t') line_start++;
        if (strncmp(line_start, "func ", 5) == 0) {
            // Копируем в clean_code все, что было до этого (предыдущие строки)
            size_t prefix_len = p - code - (clean_pos > 0 ? 1 : 0); // грубовато, проще скопировать до line_start
            // проще: запоминаем позицию начала текущей строки и копируем всё до неё
            // перепишем чище:
            const char* func_line = line_start;
            // всё, что между p и func_line (включая возможный \n), копируем
            while (p < func_line) {
                clean_code[clean_pos++] = *p++;
            }
            // теперь p указывает на func_line

            // Пропускаем "func "
            const char* after_func = p + 5;
            while (*after_func == ' ' || *after_func == '\t') after_func++;

            // Читаем имя функции
            char func_name[50];
            int i = 0;
            while (*after_func && !isspace((unsigned char)*after_func) && i < 49) {
                func_name[i++] = *after_func++;
            }
            func_name[i] = '\0';

            // Ищем конец функции: "\nend" на отдельной строке
            const char* body_start = after_func; // тело начинается после имени (может быть сразу \n)
            const char* end_ptr = NULL;
            const char* search = body_start;
            while (*search) {
                const char* line = search;
                while (*line == ' ' || *line == '\t') line++;
                if (strncmp(line, "end", 3) == 0 && (line[3] == '\n' || line[3] == '\0')) {
                    // проверяем, что это не endloop
                    if (strncmp(line, "endloop", 7) != 0) {
                        end_ptr = line;
                        break;
                    }
                }
                // переходим к следующей строке
                search = strchr(search, '\n');
                if (!search) break;
                search++;
            }
            if (!end_ptr) {
                printf("Error: no end for function %s\n", func_name);
                free(clean_code);
                return NULL;
            }

            // Генерируем определение функции в out
            fprintf(out, "void func_%s() {\n", func_name);
            // компилируем тело (от body_start до end_ptr)
            int saved_indent = current_indent;
            current_indent = 1;
            const char* body_ptr = body_start;
            // создаём временную строку тела, чтобы не испортить основной код
            size_t body_len = end_ptr - body_start;
            char* body = malloc(body_len + 1);
            memcpy(body, body_ptr, body_len);
            body[body_len] = '\0';
            const char* tmp_body = body;
            compile_block(&tmp_body, out, 1);  // вывод прямо в out
            free(body);
            fprintf(out, "}\n\n");
            current_indent = saved_indent;

            // Продвигаем p за "end" и следующий за ним \n
            p = end_ptr + 3;
            if (*p == '\n') p++;
        } else {
            // обычная строка, копируем её
            clean_code[clean_pos++] = *p++;
        }
    }
    clean_code[clean_pos] = '\0';
    return clean_code;
}

void compile_line(const char* token, FILE* out, const char** pp) {
    while (*token == ' ' || *token == '\t') token++;
    if (*token == '\0' || *token == '#') return;

    // --- классы (нужно обработать до основных конструкций) ---
    if (in_class) {
        if (strncmp(token, "let ", 4) == 0 && !strstr(token, " = new ")) {
            // свойство класса
            char* eq = strchr(token, '=');
            if (eq) {
                char name[50];
                int nl = eq - (token+4);
                while (nl>0 && isspace((unsigned char)(token+4)[nl-1])) nl--;
                strncpy(name, token+4, nl); name[nl]='\0';
                char* val = eq+1; while (isspace((unsigned char)*val)) val++;
                char escaped[512];
                escape_for_c_string(val, escaped, sizeof(escaped));
                fprintf(out, "%*salwex_class_add_property(\"%s\", \"%s\");\n", current_indent*4, "", name, escaped);
            }
            return;
        }
        if (strncmp(token, "func ", 5) == 0) {
            char name[50];
            strcpy(name, token+5);
            char* sp = name; while (*sp==' ') sp++;
            char* ep = sp; while (*ep && *ep!=' ') ep++; *ep='\0';
            const char* start = *pp;
            const char* end_ptr = strstr(start, "\nend\n");
            if (!end_ptr) end_ptr = strstr(start, "end");
            if (!end_ptr) { printf("Error: no end for method %s\n", sp); return; }
            size_t body_len = end_ptr - start;
            char* body = malloc(body_len+1);
            memcpy(body, start, body_len); body[body_len]='\0';
            if (strcmp(sp, "constructor")==0) {
                fprintf(out, "%*salwex_class_set_constructor(\"%s\");\n", current_indent*4, "", body);
            } else {
                fprintf(out, "%*salwex_class_add_method(\"%s\", \"%s\");\n", current_indent*4, "", sp, body);
            }
            free(body);
            *pp = end_ptr + 3;
            return;
        }
        if (strncmp(token, "end", 3) == 0 && strncmp(token, "endloop",7)!=0) {
            fprintf(out, "%*salwex_class_end();\n", current_indent*4, "");
            in_class = 0;
            return;
        }
        return;
    }

    // --- class (вне класса) ---
    if (strncmp(token, "class ", 6) == 0) {
        char* def = token+6; while (isspace((unsigned char)*def)) def++;
        char cname[50], pname[50]={0};
        int i=0;
        while (*def && !isspace((unsigned char)*def) && i<49) cname[i++]=*def++;
        cname[i]='\0';
        while (isspace((unsigned char)*def)) def++;
        if (strncmp(def, "extends ",8)==0) {
            def+=8; while (isspace((unsigned char)*def)) def++;
            i=0;
            while (*def && !isspace((unsigned char)*def) && i<49) pname[i++]=*def++;
            pname[i]='\0';
        }
        fprintf(out, "%*salwex_class_begin(\"%s\", \"%s\");\n", current_indent*4, "", cname, pname);
        in_class = 1;
        return;
    }

    // --- new (создание объекта) ---
    if (strncmp(token, "let ",4)==0 && strstr(token, " = new ")) {
        char* eq = strstr(token, " = new ");
        char objname[50];
        int nl = eq - (token+4);
        while (nl>0 && isspace((unsigned char)(token+4)[nl-1])) nl--;
        strncpy(objname, token+4, nl); objname[nl]='\0';
        char* cls = eq+7; while (isspace((unsigned char)*cls)) cls++;
        char classname[50];
        int i=0;
        while (*cls && !isspace((unsigned char)*cls) && i<49) classname[i++]=*cls++;
        classname[i]='\0';
        char params[256]={0};
        while (isspace((unsigned char)*cls)) cls++;
        if (*cls) {
            strncpy(params, cls, 255);
        }
        fprintf(out, "%*salwex_object_new(\"%s\", \"%s\", \"%s\");\n", current_indent*4, "", objname, classname, params);
        return;
    }

    // --- вызов метода ---
    if (strncmp(token, "call ",5)==0 && strchr(token, '.')) {
        char* objmet = token+5; while (isspace((unsigned char)*objmet)) objmet++;
        char* dot = strchr(objmet, '.');
        *dot = '\0';
        char objname[50]; strcpy(objname, objmet);
        char method[50]; strcpy(method, dot+1);
        fprintf(out, "%*salwex_object_call_method(\"%s\", \"%s\");\n", current_indent*4, "", objname, method);
        return;
    }

    // --- let obj.prop = expr ---
    if (strncmp(token, "let ",4)==0 && strchr(token, '.') && !strstr(token, " = new ")) {
        char* eq = strchr(token, '=');
        if (eq) {
            char* dot = strchr(token, '.');
            if (dot && dot < eq) {
                char objname[50];
                int nl = dot - (token+4);
                while (nl>0 && isspace((unsigned char)(token+4)[nl-1])) nl--;
                strncpy(objname, token+4, nl); objname[nl]='\0';
                char propname[50];
                char* ps = dot+1;
                int pn = eq - ps;
                while (pn>0 && isspace((unsigned char)ps[pn-1])) pn--;
                strncpy(propname, ps, pn); propname[pn]='\0';
                char* val = eq+1; while (isspace((unsigned char)*val)) val++;
                char escaped[512];
                escape_for_c_string(val, escaped, sizeof(escaped));
                fprintf(out, "%*sobject_set_property(\"%s\", \"%s\", \"%s\");\n", current_indent*4, "", objname, propname, escaped);
            }
        }
        return;
    }

    // --- let (переменные и массивы) ---
    if (strncmp(token, "let ", 4) == 0 && strchr(token, '[') && strchr(token, ']')) {
        // инициализация массива
        char* eq = strchr(token, '=');
        if (eq) {
            char arrname[50];
            int nl = eq - (token+4);
            while (nl>0 && isspace((unsigned char)(token+4)[nl-1])) nl--;
            strncpy(arrname, token+4, nl); arrname[nl]='\0';
            char* content = eq+1; while (isspace((unsigned char)*content)) content++;
            if (*content == '[') {
                content++;
                char* endb = strrchr(content, ']');
                if (endb) *endb = '\0';
                fprintf(out, "%*salwex_array_create(\"%s\");\n", current_indent*4, "", arrname);
                char* item = strtok(content, ",");
                while (item) {
                    while (isspace((unsigned char)*item)) item++;
                    if (*item == '\'' || *item == '"') {
                        char quote = *item;
                        item++;
                        char* endq = strchr(item, quote);
                        if (endq) *endq = '\0';
                        fprintf(out, "%*salwex_arr_push_string(\"%s\", \"%s\");\n", current_indent*4, "", arrname, item);
                        item = endq+1;
                    } else {
                        double num = str_to_double(item);
                        fprintf(out, "%*salwex_arr_push_number(\"%s\", %.15g);\n", current_indent*4, "", arrname, num);
                        item = item + strlen(item);
                    }
                    item = strtok(NULL, ",");
                }
            }
        }
        return;
    }

    if (strncmp(token, "let ", 4) == 0) {
        char* eq = strchr(token, '=');
        if (eq) {
            char name[50];
            int name_len = eq - (token+4);
            while (name_len > 0 && isspace((unsigned char)(token+4)[name_len-1])) name_len--;
            strncpy(name, token+4, name_len); name[name_len] = '\0';
            char* p = name;
            while (*p == ' ') p++;
            char clean_name[50];
            strcpy(clean_name, p);

            char expr[256];
            p = eq + 1;
            while (*p == ' ' || *p == '\t') p++;
            strcpy(expr, p);
            size_t len = strlen(expr);
            while (len > 0 && (expr[len-1]==' ' || expr[len-1]=='\n' || expr[len-1]=='\r')) expr[--len] = '\0';

            char escaped[512];
            escape_for_c_string(expr, escaped, sizeof(escaped));
            fprintf(out, "%*svar_assign(\"%s\", \"%s\");\n", current_indent*4, "", clean_name, escaped);
        }
        return;
    }

    // --- print ---
    if (strncmp(token, "print ", 6) == 0) {
        const char* arg = token + 6;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (*arg == '\'' || *arg == '"') {
            char quote = *arg++;
            char content[256];
            int i = 0;
            while (*arg && *arg != quote && i < 255) content[i++] = *arg++;
            content[i] = '\0';
            fprintf(out, "%*sprintf(\"%%s\\n\", \"%s\");\n", current_indent*4, "", content);
        } else if (strchr(arg, '[')) {
            char arr_name[50];
            int index = 0;
            char* br = strchr(arg, '[');
            *br = '\0';
            strncpy(arr_name, arg, sizeof(arr_name)-1);
            index = atoi(br+1);
            fprintf(out, "%*sarray_print(\"%s\", %d);\n", current_indent*4, "", arr_name, index);
        } else if (strchr(arg, '.')) {
            char obj_name[50], prop_name[50];
            char* dot = strchr(arg, '.');
            *dot = '\0';
            strncpy(obj_name, arg, sizeof(obj_name)-1);
            strncpy(prop_name, dot+1, sizeof(prop_name)-1);
            fprintf(out, "%*sobject_print_property(\"%s\", \"%s\");\n", current_indent*4, "", obj_name, prop_name);
        } else {
            fprintf(out, "%*svar_print(\"%s\");\n", current_indent*4, "", arg);
        }
        return;
    }

    // ---------- новые статические переменные (внутри compile_line или глобально) ----------
    static int if_stack[100] = {0};  // 1 – открыта скобка на этом уровне, 0 – нет
    static int if_depth = 0;

    // ---------- замени существующую обработку if/elif/else/end на этот блок ----------
    if (strncmp(token, "if ", 3) == 0) {
        char cond[200];
        strcpy(cond, token+3);
        fprintf(out, "%*sif (eval_condition(\"%s\")) {\n", current_indent*4, "", cond);
        if_stack[if_depth] = 1;
        if_depth++;
        current_indent++;
        return;
    }
    if (strncmp(token, "elif ", 5) == 0) {
        if (if_depth == 0) { printf("Error: elif without if\n"); return; }
        if (if_stack[if_depth-1]) {
            fprintf(out, "%*s}\n", (current_indent-1)*4, "");
        }
        char cond[200];
        strcpy(cond, token+5);
        fprintf(out, "%*selse if (eval_condition(\"%s\")) {\n", (current_indent-1)*4, "", cond);
        if_stack[if_depth-1] = 1;
        return;
    }
    if (strncmp(token, "else if ", 8) == 0) {
        if (if_depth == 0) { printf("Error: else if without if\n"); return; }
        if (if_stack[if_depth-1]) {
            fprintf(out, "%*s}\n", (current_indent-1)*4, "");
        }
        char cond[200];
        strcpy(cond, token+8);
        fprintf(out, "%*selse if (eval_condition(\"%s\")) {\n", (current_indent-1)*4, "", cond);
        if_stack[if_depth-1] = 1;
        return;
    }
    if (strncmp(token, "else", 4) == 0 && (token[4] == '\0' || isspace((unsigned char)token[4]))) {
        if (if_depth == 0) { printf("Error: else without if\n"); return; }
        if (if_stack[if_depth-1]) {
            fprintf(out, "%*s}\n", (current_indent-1)*4, "");
        }
        fprintf(out, "%*selse {\n", (current_indent-1)*4, "");
        if_stack[if_depth-1] = 1;
        return;
    }
    if (strncmp(token, "end", 3) == 0 && strncmp(token, "endloop", 7) != 0) {
        if (if_depth > 0) {
            if (if_stack[if_depth-1]) {
                fprintf(out, "%*s}\n", (current_indent-1)*4, "");
            }
            if_depth--;
            current_indent--;
        }
        // иначе end может закрывать класс или функцию (эти случаи обрабатываются в других местах)
        return;
    }

    // --- while ---
    if (strncmp(token, "while ", 6) == 0) {
        char cond[200];
        strcpy(cond, token+6);
        fprintf(out, "%*swhile (eval_condition(\"%s\")) {\n", current_indent*4, "", cond);
        current_indent++;
        compile_block(pp, out, 0);  // тело цикла до endloop
        fprintf(out, "%*s}\n", current_indent*4, "");
        current_indent--;
        return;
    }

    // --- endloop (ничего не делаем, выход произошёл в compile_block) ---
    if (strncmp(token, "endloop", 7) == 0) return;

    // --- call ---
    if (strncmp(token, "call ", 5) == 0) {
        char name[50];
        strcpy(name, token+5);
        char* p = name;
        while (*p == ' ') p++;
        char* e = p;
        while (*e && *e != ' ') e++;
        *e = '\0';
        fprintf(out, "%*sfunc_%s();\n", current_indent*4, "", p);
        return;
    }

    // import ...
    if (strncmp(token, "import ", 7) == 0) {
        char lib[100];
        strcpy(lib, token+7);
        while (*lib == ' ') memmove(lib, lib+1, strlen(lib));
        fprintf(out, "%*simport_library(\"%s\", 0);\n", current_indent*4, "", lib);
        return;
    }

    // wait ...
    if (strncmp(token, "wait ", 5) == 0) {
        int sec = atoi(token+5);
        fprintf(out, "%*ssleep(%d);\n", current_indent*4, "", sec);
        return;
    }

    // http_get ...
    if (strncmp(token, "http_get ", 9) == 0) {
        const char* url = token + 9;
        while (*url == ' ' || *url == '\t') url++;
        char clean_url[256] = {0};
        if (*url == '\'' || *url == '"') {
            char quote = *url++;
            const char* endq = strchr(url, quote);
            if (endq) {
                size_t len = endq - url;
                if (len >= sizeof(clean_url)) len = sizeof(clean_url)-1;
                strncpy(clean_url, url, len);
                clean_url[len] = '\0';
            } else {
                strcpy(clean_url, url);
            }
        } else {
            strcpy(clean_url, url);
        }
        fprintf(out, "%*shttp_get(\"%s\");\n", current_indent*4, "", clean_url);
        return;
    }

        // inp ...
    if (strncmp(token, "inp ", 4) == 0) {
        char* args = token + 4;
        char type[16] = {0};
        char var_name[50] = {0};
        char prompt[256] = {0};

        sscanf(args, "%15s %49s", type, var_name);

        char* quote_start = strchr(token, '\'');
        if (quote_start) {
            char* quote_end = strrchr(token, '\'');
            if (quote_end && quote_end > quote_start) {
                int prompt_len = quote_end - quote_start - 1;
                if (prompt_len >= (int)sizeof(prompt)) prompt_len = sizeof(prompt)-1;
                strncpy(prompt, quote_start+1, prompt_len);
                prompt[prompt_len] = '\0';
            }
        }

        if (strcmp(type, "string") == 0) {
            fprintf(out, "%*salwex_input_string(\"%s\", \"%s\");\n", current_indent*4, "", prompt, var_name);
        } else if (strcmp(type, "int") == 0 || strcmp(type, "float") == 0) {
            fprintf(out, "%*salwex_input_number(\"%s\", \"%s\");\n", current_indent*4, "", prompt, var_name);
        }
        return;
    }

    // arr_get arr index
    if (strncmp(token, "arr_get ", 8) == 0) {
        char* args = token + 8;
        char arr_name[50];
        int index;
        sscanf(args, "%49s %d", arr_name, &index);
        fprintf(out, "%*salwex_arr_get(\"%s\", %d);\n", current_indent*4, "", arr_name, index);
        return;
    }

    // arr_set arr index value
    if (strncmp(token, "arr_set ", 8) == 0) {
        char* args = token + 8;
        char arr_name[50];
        int index;
        double value;
        sscanf(args, "%49s %d %lf", arr_name, &index, &value);
        fprintf(out, "%*salwex_arr_set(\"%s\", %d, %.15g);\n", current_indent*4, "", arr_name, index, value);
        return;
    }

    // arr_push arr value
    if (strncmp(token, "arr_push ", 9) == 0) {
        char* args = token + 9;
        char arr_name[50];
        // value может быть числом или строкой
        char* space = strchr(args, ' ');
        if (space) {
            int name_len = space - args;
            if (name_len >= 50) name_len = 49;
            strncpy(arr_name, args, name_len);
            arr_name[name_len] = '\0';
            char* val_str = space + 1;
            while (*val_str == ' ' || *val_str == '\t') val_str++;
            if (*val_str == '\'' || *val_str == '"') {
                char quote = *val_str;
                val_str++;
                char* endq = strchr(val_str, quote);
                if (endq) *endq = '\0';
                fprintf(out, "%*salwex_arr_push_string(\"%s\", \"%s\");\n", current_indent*4, "", arr_name, val_str);
            } else {
                double num = atof(val_str);
                fprintf(out, "%*salwex_arr_push_number(\"%s\", %.15g);\n", current_indent*4, "", arr_name, num);
            }
        }
        return;
    }

    // arr_length arr
    if (strncmp(token, "arr_length ", 11) == 0) {
        char* args = token + 11;
        while (*args == ' ' || *args == '\t') args++;
        fprintf(out, "%*salwex_arr_length(\"%s\");\n", current_indent*4, "", args);
        return;
    }

    // инициализация массива let arr = [...]
    if (strncmp(token, "let ",4)==0 && strchr(token, '[') && strchr(token, ']')) {
        char* eq = strchr(token, '=');
        if (eq) {
            char arrname[50];
            int nl = eq - (token+4);
            while (nl>0 && isspace((unsigned char)(token+4)[nl-1])) nl--;
            strncpy(arrname, token+4, nl); arrname[nl]='\0';
            char* content = eq+1; while (isspace(*content)) content++;
            if (*content == '[') {
                content++;
                // удаляем ']' в конце
                char* endb = strrchr(content, ']');
                if (endb) *endb = '\0';
                // создаём массив
                fprintf(out, "%*salwex_array_create(\"%s\");\n", current_indent*4, "", arrname);
                // парсим элементы
                char* item = strtok(content, ",");
                while (item) {
                    while (isspace(*item)) item++;
                    if (*item == '\'' || *item == '"') {
                        char quote = *item;
                        item++;
                        char* endq = strchr(item, quote);
                        if (endq) *endq = '\0';
                        fprintf(out, "%*salwex_arr_push_string(\"%s\", \"%s\");\n", current_indent*4, "", arrname, item);
                        item = endq+1;
                    } else {
                        double num = str_to_double(item); // нужно иметь эту функцию в компиляторе или использовать atof
                        fprintf(out, "%*salwex_arr_push_number(\"%s\", %.15g);\n", current_indent*4, "", arrname, num);
                        item = item + strlen(item);
                    }
                    item = strtok(NULL, ",");
                }
            }
        }
        return;
}

    // file_write filename content
    if (strncmp(token, "file_write ", 11) == 0) {
        const char* args = token + 11;
        while (*args == ' ' || *args == '\t') args++;
        char filename[100];
        int i = 0;
        while (*args && !my_isspace(*args) && i < 99) filename[i++] = *args++;
        filename[i] = '\0';
        while (*args == ' ' || *args == '\t') args++;
    
        char escaped[1024];
        escape_for_c_string(args, escaped, sizeof(escaped));
        fprintf(out, "%*s{\n", current_indent*4, "");
        fprintf(out, "%*schar alwex_file_content[1024];\n", current_indent*4, "");
        fprintf(out, "%*sexpand_vars(alwex_file_content, \"%s\", sizeof(alwex_file_content));\n", current_indent*4, "", escaped);
        fprintf(out, "%*sFILE* f = fopen(\"%s\", \"w\");\n", current_indent*4, "", filename);
        fprintf(out, "%*sif (f) { fputs(alwex_file_content, f); fclose(f); }\n", current_indent*4, "");
        fprintf(out, "%*s}\n", current_indent*4, "");
        return;
    }

    // file_read filename
    if (strncmp(token, "file_read ", 10) == 0) {
        const char* filename = token + 10;
        while (*filename == ' ' || *filename == '\t') filename++;
        fprintf(out, "%*s{\n", current_indent*4, "");
        fprintf(out, "%*sFILE* f = fopen(\"%s\", \"r\");\n", current_indent*4, "", filename);
        fprintf(out, "%*sif (f) {\n", current_indent*4, "");
        fprintf(out, "%*sfseek(f, 0, SEEK_END);\n", current_indent*4, "");
        fprintf(out, "%*slong size = ftell(f);\n", current_indent*4, "");
        fprintf(out, "%*srewind(f);\n", current_indent*4, "");
        fprintf(out, "%*schar* content = malloc(size + 1);\n", current_indent*4, "");
        fprintf(out, "%*sif (content) {\n", current_indent*4, "");
        fprintf(out, "%*sfread(content, 1, size, f);\n", current_indent*4, "");
        fprintf(out, "%*scontent[size] = '\\0';\n", current_indent*4, "");
        fprintf(out, "%*sstruct Variable* v = find_variable(\"file_content\");\n", current_indent*4, "");
        fprintf(out, "%*sif (!v) {\n", current_indent*4, "");
        fprintf(out, "%*sint idx = add_variable();\n", current_indent*4, "");
        fprintf(out, "%*sif (idx >= 0) {\n", current_indent*4, "");
        fprintf(out, "%*sv = &variables[idx];\n", current_indent*4, "");
        fprintf(out, "%*sstrcpy(v->name, \"file_content\");\n", current_indent*4, "");
        fprintf(out, "%*s}\n", current_indent*4, "");
        fprintf(out, "%*s}\n", current_indent*4, "");
        fprintf(out, "%*sif (v) {\n", current_indent*4, "");
        fprintf(out, "%*sint si = add_string();\n", current_indent*4, "");
        fprintf(out, "%*sif (si >= 0) {\n", current_indent*4, "");
        fprintf(out, "%*sstrcpy(string_pool[si], content);\n", current_indent*4, "");
        fprintf(out, "%*sv->str_value = string_pool[si];\n", current_indent*4, "");
        fprintf(out, "%*sv->value = size;\n", current_indent*4, "");
        fprintf(out, "%*s}\n", current_indent*4, "");
        fprintf(out, "%*s}\n", current_indent*4, "");
        fprintf(out, "%*sfree(content);\n", current_indent*4, "");
        fprintf(out, "%*s}\n", current_indent*4, "");
        fprintf(out, "%*sfclose(f);\n", current_indent*4, "");
        fprintf(out, "%*s}\n", current_indent*4, "");
        return;
    }

    // file_append filename content
    if (strncmp(token, "file_append ", 12) == 0) {
        const char* args = token + 12;
        while (*args == ' ' || *args == '\t') args++;
        char filename[100];
        int i = 0;
        while (*args && !isspace((unsigned char)*args) && i < 99) filename[i++] = *args++;
        filename[i] = '\0';
        while (*args == ' ' || *args == '\t') args++;

        char content[1024] = {0};
        if (*args == '\'' || *args == '"') {
            char quote = *args++;
            const char* endq = strchr(args, quote);
            if (endq) {
                size_t len = endq - args;
                if (len >= sizeof(content)) len = sizeof(content)-1;
                strncpy(content, args, len);
                content[len] = '\0';
            } else {
                strcpy(content, args);
            }
        } else {
            strcpy(content, args);
        }
        char escaped[1024];
        escape_for_c_string(content, escaped, sizeof(escaped));
        fprintf(out, "%*salwex_file_append(\"%s\", \"%s\");\n", current_indent*4, "", filename, escaped);
        return;
    }

    // file_exists filename
    if (strncmp(token, "file_exists ", 12) == 0) {
        char* filename = token + 12;
        while (*filename == ' ' || *filename == '\t') filename++;
        fprintf(out, "%*salwex_file_exists(\"%s\");\n", current_indent*4, "", filename);
        return;
    }

    // exec command
    if (strncmp(token, "exec ", 5) == 0) {
        const char* cmd = token + 5;
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        char escaped[1024];
        escape_for_c_string(cmd, escaped, sizeof(escaped));
        fprintf(out, "%*s{\n", current_indent*4, "");
        fprintf(out, "%*schar alwex_exec_cmd[1024];\n", current_indent*4, "");
        fprintf(out, "%*sexpand_vars(alwex_exec_cmd, \"%s\", sizeof(alwex_exec_cmd));\n", current_indent*4, "", escaped);
        fprintf(out, "%*ssystem(alwex_exec_cmd);\n", current_indent*4, "");
        fprintf(out, "%*s}\n", current_indent*4, "");
        return;
    }

    // http_post url data
    if (strncmp(token, "http_post ", 10) == 0) {
        const char* args = token + 10;
        while (*args == ' ' || *args == '\t') args++;
        char url[256] = {0};
        char data[512] = {0};

        // читаем URL
        if (*args == '\'' || *args == '"') {
            char quote = *args++;
            const char* endq = strchr(args, quote);
            if (endq) {
                size_t len = endq - args;
                if (len >= sizeof(url)) len = sizeof(url)-1;
                strncpy(url, args, len);
                url[len] = '\0';
                args = endq + 1;
            }
        } else {
            // без кавычек – читаем до пробела
            int i = 0;
            while (*args && !isspace((unsigned char)*args) && i < 255) url[i++] = *args++;
            url[i] = '\0';
        }

        while (*args == ' ' || *args == '\t') args++;

        // читаем данные
        if (*args == '\'' || *args == '"') {
            char quote = *args++;
            const char* endq = strchr(args, quote);
            if (endq) {
                size_t len = endq - args;
                if (len >= sizeof(data)) len = sizeof(data)-1;
                strncpy(data, args, len);
                data[len] = '\0';
            }
        } else {
            int i = 0;
            while (*args && !isspace((unsigned char)*args) && i < 511) data[i++] = *args++;
            data[i] = '\0';
        }

        fprintf(out, "%*shttp_post(\"%s\", \"%s\");\n", current_indent*4, "", url, data);
        return;
    }

    // http_download url filename
    if (strncmp(token, "http_download ", 14) == 0) {
        const char* args = token + 14;
        while (*args == ' ' || *args == '\t') args++;
        char url[256] = {0};
        char filename[100] = {0};

        // читаем URL
        if (*args == '\'' || *args == '"') {
            char quote = *args++;
            const char* endq = strchr(args, quote);
            if (endq) {
                size_t len = endq - args;
                if (len >= sizeof(url)) len = sizeof(url)-1;
                strncpy(url, args, len);
                url[len] = '\0';
                args = endq + 1;
            }
        } else {
            int i = 0;
            while (*args && !isspace((unsigned char)*args) && i < 255) url[i++] = *args++;
            url[i] = '\0';
        }

        while (*args == ' ' || *args == '\t') args++;

        // читаем имя файла
        if (*args == '\'' || *args == '"') {
            char quote = *args++;
            const char* endq = strchr(args, quote);
            if (endq) {
                size_t len = endq - args;
                if (len >= sizeof(filename)) len = sizeof(filename)-1;
                strncpy(filename, args, len);
                filename[len] = '\0';
            }
        } else {
            int i = 0;
            while (*args && !isspace((unsigned char)*args) && i < 99) filename[i++] = *args++;
            filename[i] = '\0';
        }

        fprintf(out, "%*shttp_download(\"%s\", \"%s\");\n", current_indent*4, "", url, filename);
        return;
    }

    // ++ и --
    if (strstr(token, "++") || strstr(token, "--")) {
        char var_name[50] = {0};
        int is_inc = strstr(token, "++") != NULL;
        char* start = token;
        while (*start == ' ' || *start == '\t') start++;
        int i = 0;
        while (*start && (isalnum(*start) || *start == '_') && i < 49) {
            var_name[i++] = *start++;
        }
        var_name[i] = '\0';
        fprintf(out, "%*salwex_%s(\"%s\");\n", current_indent*4, "", is_inc ? "inc_var" : "dec_var", var_name);
        return;
    }

    // str_split var_name delimiter array_name
    if (strncmp(token, "str_split ", 10) == 0) {
        char* args = token + 10;
        char str_var[50], delim_raw[50]={0}, arr_name[50];
        // читаем str_var
        int n = 0;
        char* p = args;
        while (isspace(*p)) p++;
        char* start = p;
        while (*p && !isspace(*p)) p++;
        if (p > start) { strncpy(str_var, start, p-start); str_var[p-start]='\0'; n++; }
        while (isspace(*p)) p++;
        // читаем delim (может быть в кавычках)
        if (*p == '\'' || *p == '"') {
            char quote = *p++;
            start = p;
            while (*p && *p != quote) p++;
            if (*p == quote) {
                strncpy(delim_raw, start, p-start);
                delim_raw[p-start] = '\0';
                p++;
                n++;
            }
        } else {
            start = p;
            while (*p && !isspace(*p)) p++;
            strncpy(delim_raw, start, p-start);
            delim_raw[p-start] = '\0';
            n++;
        }
        while (isspace(*p)) p++;
        // читаем arr_name
        start = p;
        while (*p && !isspace(*p)) p++;
        if (p > start) { strncpy(arr_name, start, p-start); arr_name[p-start]='\0'; n++; }

        if (n == 3) {
            fprintf(out, "%*salwex_str_split(\"%s\", \"%s\", \"%s\");\n", current_indent*4, "", str_var, delim_raw, arr_name);
        }
        return;
    }
}

void generate_program(const char* code, FILE* out) {
    fprintf(out, "#include \"alwex_runtime.h\"\n\n");

    // Первый проход: извлекаем и компилируем функции
    char* cleaned_code = extract_and_compile_functions(code, out);
    if (!cleaned_code) {
        fprintf(stderr, "Error extracting functions\n");
        return;
    }

    // Пишем main
    fprintf(out, "int main() {\n");
    fprintf(out, "    init_memory();\n");

    current_indent = 1;
    const char* p = cleaned_code;
    compile_block(&p, out, 0);

    fprintf(out, "    free_memory();\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");

    free(cleaned_code);
}

// ---------- вызов GCC ----------
void run_gcc(const char* c_filename, const char* out_name) {
    char cmd[1024];
    // Ищем gcc сначала в папке с компилятором, потом в PATH
    char gcc_path[512];
    #ifdef _WIN32
        GetModuleFileNameA(NULL, gcc_path, sizeof(gcc_path));
        char* last_sep = strrchr(gcc_path, '\\');
        if (last_sep) *last_sep = '\0';
        strcat(gcc_path, "\\gcc.exe");
        if (GetFileAttributesA(gcc_path) == INVALID_FILE_ATTRIBUTES)
            strcpy(gcc_path, "gcc");  // fallback to PATH
    #else
        ssize_t len = readlink("/proc/self/exe", gcc_path, sizeof(gcc_path)-1);
        if (len != -1) {
            gcc_path[len] = '\0';
            char* slash = strrchr(gcc_path, '/');
            if (slash) *slash = '\0';
            strcat(gcc_path, "/gcc");
            if (access(gcc_path, X_OK) != 0)
                strcpy(gcc_path, "gcc");
        } else {
            strcpy(gcc_path, "gcc");
        }
    #endif

    #ifdef _WIN32
        snprintf(cmd, sizeof(cmd),
            "\"%s\" %s alwex_runtime.o -o %s.exe -lwininet -Wall -Wextra",
            gcc_path, c_filename, out_name);
    #else
        snprintf(cmd, sizeof(cmd),
            "\"%s\" %s alwex_runtime.o -o %s -lcurl -Wall -Wextra",
            gcc_path, c_filename, out_name);
    #endif

    printf("Compiling C code...\n%s\n", cmd);
    int ret = system(cmd);
    if (ret != 0) {
        printf("Error: gcc compilation failed.\n");
        exit(1);
    }
    // удаляем временный .c файл (опционально)
    remove(c_filename);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Alwex Compiler\nUsage: %s <script.alw> [output_name]\n", argv[0]);
        return 1;
    }

    const char* input_file = argv[1];
    char output_name[256];
    if (argc >= 3) {
        strncpy(output_name, argv[2], 255);
    } else {
        strncpy(output_name, input_file, 255);
        char* dot = strrchr(output_name, '.');
        if (dot) *dot = '\0';
    }

    // читаем исходник
    FILE* f = fopen(input_file, "r");
    if (!f) {
        perror("fopen input");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char* code = malloc(size+1);
    fread(code, 1, size, f);
    code[size] = '\0';
    fclose(f);

    // генерируем C файл
    char c_filename[256];
    snprintf(c_filename, sizeof(c_filename), "%s.c", output_name);
    FILE* c_out = fopen(c_filename, "w");
    if (!c_out) {
        perror("fopen output C file");
        free(code);
        return 1;
    }
    generate_program(code, c_out);
    fclose(c_out);
    free(code);

    // компилируем C в исполняемый файл
    run_gcc(c_filename, output_name);

    printf("Successfully compiled to %s\n", output_name);
    return 0;
}
