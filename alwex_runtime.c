#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

#ifdef _WIN32
    #include <windows.h>
    #include <wininet.h>
    #include <direct.h>
    #pragma comment(lib, "wininet.lib")
    #define sleep(seconds) Sleep(seconds * 1000)
#else
    #include <unistd.h>
    #include <curl/curl.h>
#endif

#include "alwex_runtime.h"

#define MAX_FUNC_BODY_SIZE 4096
#define MAX_LINE_LEN 1024
#define MAX_HTTP_RESPONSE 1048576  // 1MB max response

#ifndef _WIN32
#define strlcpy(dest, src, size) strncpy(dest, src, size)
#else
size_t strlcpy(char *dest, const char *src, size_t size) {
    size_t src_len = strlen(src);
    if (size == 0) return src_len;
    
    size_t to_copy = src_len < size - 1 ? src_len : size - 1;
    memcpy(dest, src, to_copy);
    dest[to_copy] = '\0';
    return src_len;
}
#endif

static unsigned long rand_state = 0;
static char g_current_context_object[50] = {0};

struct Variable *variables = NULL;
int var_count = 0;
static int var_capacity = 0;

char **string_pool = NULL;
int string_count = 0;
static int string_capacity = 0;

struct Array *arrays = NULL;
int array_count = 0;
static int array_capacity = 0;

struct Function *functions = NULL;
int function_count = 0;
static int function_capacity = 0;

struct Class *classes = NULL;
int class_count = 0;
static int class_capacity = 0;

struct Object *objects = NULL;
int object_count = 0;
static int object_capacity = 0;

struct HttpResponse last_http_response = {NULL, 0};
char current_script_dir[512] = {0};
char interpreter_dir[512] = {0};

void init_memory();
void free_memory();
int add_variable();
int add_string();
int add_function();
int file_exists(const char* path);
int dir_exists(const char* path);

// HTTP functions
#ifndef _WIN32
size_t alwex_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct HttpResponse *mem = (struct HttpResponse *)userp;
    
    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if(!ptr) {
        printf("Error: not enough memory for HTTP response\n");
        return 0;
    }
    
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;
    
    return realsize;
}
#endif

void http_get(const char* url) {
#ifdef _WIN32
    HINTERNET hInternet = InternetOpenA("AlwexScript", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInternet) {
        printf("Error: cannot initialize WinINet\n");
        return;
    }
    
    HINTERNET hConnect = InternetOpenUrlA(hInternet, url, NULL, 0, INTERNET_FLAG_RELOAD, 0);
    if (!hConnect) {
        printf("Error: cannot connect to URL\n");
        InternetCloseHandle(hInternet);
        return;
    }
    
    if (last_http_response.data) free(last_http_response.data);
    last_http_response.data = malloc(MAX_HTTP_RESPONSE);
    last_http_response.size = 0;
    
    DWORD bytesRead = 0;
    char buffer[4096];
    
    while (InternetReadFile(hConnect, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        if (last_http_response.size + bytesRead < MAX_HTTP_RESPONSE) {
            memcpy(last_http_response.data + last_http_response.size, buffer, bytesRead);
            last_http_response.size += bytesRead;
        }
    }
    
    last_http_response.data[last_http_response.size] = '\0';
    
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);
#else
    CURL *curl = curl_easy_init();
    if(!curl) {
        printf("Error: cannot initialize curl\n");
        return;
    }
    
    if (last_http_response.data) free(last_http_response.data);
    last_http_response.data = malloc(1);
    last_http_response.size = 0;
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, alwex_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&last_http_response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "AlwexScript");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    
    CURLcode res = curl_easy_perform(curl);
    if(res != CURLE_OK) {
        printf("Error: HTTP GET failed: %s\n", curl_easy_strerror(res));
    }
    
    curl_easy_cleanup(curl);
#endif
}

void http_post(const char* url, const char* data) {
#ifdef _WIN32
    HINTERNET hInternet = InternetOpenA("AlwexScript", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInternet) {
        printf("Error: cannot initialize WinINet\n");
        return;
    }
    
    URL_COMPONENTSA urlComponents;
    char hostname[256], path[1024];
    memset(&urlComponents, 0, sizeof(urlComponents));
    urlComponents.dwStructSize = sizeof(urlComponents);
    urlComponents.lpszHostName = hostname;
    urlComponents.dwHostNameLength = sizeof(hostname);
    urlComponents.lpszUrlPath = path;
    urlComponents.dwUrlPathLength = sizeof(path);
    
    if (!InternetCrackUrlA(url, 0, 0, &urlComponents)) {
        printf("Error: invalid URL\n");
        InternetCloseHandle(hInternet);
        return;
    }
    
    HINTERNET hConnect = InternetConnectA(hInternet, hostname, urlComponents.nPort, 
                                          NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConnect) {
        printf("Error: cannot connect to server\n");
        InternetCloseHandle(hInternet);
        return;
    }
    
    HINTERNET hRequest = HttpOpenRequestA(hConnect, "POST", path, NULL, NULL, NULL,
                                          INTERNET_FLAG_RELOAD, 0);
    if (!hRequest) {
        printf("Error: cannot create request\n");
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInternet);
        return;
    }
    
    const char* headers = "Content-Type: application/x-www-form-urlencoded\r\n";
    BOOL result = HttpSendRequestA(hRequest, headers, strlen(headers), (LPVOID)data, strlen(data));
    
    if (result) {
        if (last_http_response.data) free(last_http_response.data);
        last_http_response.data = malloc(MAX_HTTP_RESPONSE);
        last_http_response.size = 0;
        
        DWORD bytesRead = 0;
        char buffer[4096];
        
        while (InternetReadFile(hRequest, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
            if (last_http_response.size + bytesRead < MAX_HTTP_RESPONSE) {
                memcpy(last_http_response.data + last_http_response.size, buffer, bytesRead);
                last_http_response.size += bytesRead;
            }
        }
        
        last_http_response.data[last_http_response.size] = '\0';
    }
    
    InternetCloseHandle(hRequest);
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);
#else
    CURL *curl = curl_easy_init();
    if(!curl) {
        printf("Error: cannot initialize curl\n");
        return;
    }
    
    if (last_http_response.data) free(last_http_response.data);
    last_http_response.data = malloc(1);
    last_http_response.size = 0;
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, alwex_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&last_http_response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "AlwexScript");
    
    CURLcode res = curl_easy_perform(curl);
    if(res != CURLE_OK) {
        printf("Error: HTTP POST failed: %s\n", curl_easy_strerror(res));
    }
    
    curl_easy_cleanup(curl);
#endif
}

void http_download(const char* url, const char* filename) {
#ifdef _WIN32
    HINTERNET hInternet = InternetOpenA("AlwexScript", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInternet) {
        printf("Error: cannot initialize WinINet\n");
        return;
    }
    
    HINTERNET hConnect = InternetOpenUrlA(hInternet, url, NULL, 0, INTERNET_FLAG_RELOAD, 0);
    if (!hConnect) {
        printf("Error: cannot connect to URL\n");
        InternetCloseHandle(hInternet);
        return;
    }
    
    FILE* file = fopen(filename, "wb");
    if (!file) {
        printf("Error: cannot create file %s\n", filename);
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInternet);
        return;
    }
    
    DWORD bytesRead = 0;
    char buffer[4096];
    
    while (InternetReadFile(hConnect, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        fwrite(buffer, 1, bytesRead, file);
    }
    
    fclose(file);
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);
    
    printf("File downloaded: %s\n", filename);
#else
    CURL *curl = curl_easy_init();
    if(!curl) {
        printf("Error: cannot initialize curl\n");
        return;
    }
    
    FILE *fp = fopen(filename, "wb");
    if(!fp) {
        printf("Error: cannot create file %s\n", filename);
        curl_easy_cleanup(curl);
        return;
    }
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    
    CURLcode res = curl_easy_perform(curl);
    if(res != CURLE_OK) {
        printf("Error: download failed: %s\n", curl_easy_strerror(res));
    } else {
        printf("File downloaded: %s\n", filename);
    }
    
    fclose(fp);
    curl_easy_cleanup(curl);
#endif
}

int alwex_rand() {
    rand_state = rand_state * 1103515245 + 12345;
    return (unsigned int)(rand_state / 65536) % 32768;
}

void alwex_srand(unsigned int seed) {
    rand_state = seed;
}

void alwex_input_string(const char* prompt, const char* var_name) {
    if (prompt && strlen(prompt) > 0) {
        printf("%s", prompt);
        fflush(stdout);
    }
    char buf[256];
    if (fgets(buf, sizeof(buf), stdin)) {
        // удаляем \n
        size_t len = strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
        // создаём строку в пуле
        int idx = add_string();
        if (idx >= 0) {
            strncpy(string_pool[idx], buf, STRING_SIZE-1);
            string_pool[idx][STRING_SIZE-1] = '\0';
            // присваиваем переменной через var_assign с кавычками
            char expr[STRING_SIZE+3];
            snprintf(expr, sizeof(expr), "'%s'", buf);
            var_assign(var_name, expr);
        }
    }
}

void alwex_input_number(const char* prompt, const char* var_name) {
    if (prompt && strlen(prompt) > 0) {
        printf("%s", prompt);
        fflush(stdout);
    }
    double val;
    if (scanf("%lf", &val) == 1) {
        // очистить оставшийся \n
        while (getchar() != '\n');
        char expr[64];
        snprintf(expr, sizeof(expr), "%.15g", val);
        var_assign(var_name, expr);
    }
}

void alwex_str_from_number(double num, char* out, size_t out_size) {
    snprintf(out, out_size, "%.15g", num);
}

void alwex_arr_get(const char* arr_name, int index) {
    struct Array* arr = find_array(arr_name);
    if (!arr || index < 0 || index >= arr->size) {
        printf("Error: array index out of bounds\n");
        return;
    }
    struct Variable* v = find_variable("arr_get_result");
    if (!v) {
        int idx = add_variable();
        if (idx < 0) return;
        v = &variables[idx];
        strcpy(v->name, "arr_get_result");
    }
    if (arr->is_string_array) {
        int si = add_string();
        if (si >= 0) {
            strcpy(string_pool[si], arr->strings[index]);
            v->str_value = string_pool[si];
            v->value = 0;
        }
    } else {
        v->value = arr->values[index];
        v->str_value = NULL;
    }
}

void alwex_arr_set(const char* arr_name, int index, double value) {
    struct Array* arr = find_array(arr_name);
    if (arr && index >= 0 && index < arr->size && !arr->is_string_array) {
        arr->values[index] = value;
    }
}

void alwex_arr_push_number(const char* arr_name, double value) {
    struct Array* arr = find_array(arr_name);
    if (!arr) {
        int idx = add_array();
        if (idx < 0) return;
        arr = &arrays[idx];
        strcpy(arr->name, arr_name);
        arr->size = 0;
        arr->is_string_array = 0;
    }
    if (arr->size < MAX_ARRAY_SIZE) {
        arr->values[arr->size++] = value;
    }
}

void alwex_arr_push_string(const char* arr_name, const char* value) {
    struct Array* arr = find_array(arr_name);
    if (!arr) {
        int idx = add_array();
        if (idx < 0) return;
        arr = &arrays[idx];
        strcpy(arr->name, arr_name);
        arr->size = 0;
        arr->is_string_array = 1;
    }
    if (arr->size < MAX_ARRAY_SIZE) {
        int si = add_string();
        if (si >= 0) {
            strcpy(string_pool[si], value);
            arr->strings[arr->size] = string_pool[si];
            arr->is_string_array = 1;
            arr->size++;
        }
    }
}

void alwex_arr_length(const char* arr_name) {
    struct Array* arr = find_array(arr_name);
    struct Variable* v = find_variable("arr_length_result");
    if (!v) {
        int idx = add_variable();
        if (idx < 0) return;
        v = &variables[idx];
        strcpy(v->name, "arr_length_result");
    }
    v->value = arr ? arr->size : 0;
    v->str_value = NULL;
}

void alwex_file_write(const char* filename, const char* content) {
    FILE* f = fopen(filename, "w");
    if (f) { fputs(content, f); fclose(f); }
    else printf("Error: cannot write to file %s\n", filename);
}

void alwex_file_read(const char* filename) {
    FILE* f = fopen(filename, "r");
    if (f) {
        char buf[256];
        while (fgets(buf, sizeof(buf), f)) printf("%s", buf);
        fclose(f);
        printf("\n");
    } else printf("Error: cannot read file %s\n", filename);
}

void alwex_file_append(const char* filename, const char* content) {
    FILE* f = fopen(filename, "a");
    if (f) { fputs(content, f); fclose(f); }
    else printf("Error: cannot append to file %s\n", filename);
}

void alwex_file_exists(const char* filename) {
    printf("%s\n", file_exists(filename) ? "true" : "false");
}

void alwex_inc_var(const char* name) {
    struct Variable* v = find_variable(name);
    if (v) v->value += 1.0;
}

void alwex_dec_var(const char* name) {
    struct Variable* v = find_variable(name);
    if (v) v->value -= 1.0;
}

void alwex_str_split(const char* var_name, const char* delim, const char* arr_name) {
    struct Variable* sv = find_variable(var_name);
    if (!sv || !sv->str_value) return;
    struct Array* arr = find_array(arr_name);
    if (!arr) {
        int idx = add_array();
        if (idx < 0) return;
        arr = &arrays[idx];
        strcpy(arr->name, arr_name);
        arr->size = 0;
        arr->is_string_array = 1;
    } else {
        arr->size = 0;
        arr->is_string_array = 1;
    }
    char* copy = strdup(sv->str_value);
    char* token = strtok(copy, delim);
    while (token && arr->size < MAX_ARRAY_SIZE) {
        int si = add_string();
        if (si >= 0) {
            strcpy(string_pool[si], token);
            arr->strings[arr->size++] = string_pool[si];
        }
        token = strtok(NULL, delim);
    }
    free(copy);
}

// ---------- обёртки для классов (компилятор) ----------
static struct Class* current_compiled_class = NULL;

void alwex_class_begin(const char* name, const char* parent) {
    int idx = add_class();
    if (idx < 0) return;
    current_compiled_class = &classes[idx];
    strcpy(current_compiled_class->name, name);
    if (parent && strlen(parent) > 0) {
        strcpy(current_compiled_class->parent_name, parent);
        struct Class* pclass = find_class(parent);
        if (pclass) {
            // наследуем свойства и методы
            for (int i = 0; i < pclass->property_count && current_compiled_class->property_count < MAX_CLASS_PROPERTIES; i++) {
                current_compiled_class->properties[current_compiled_class->property_count] = pclass->properties[i];
                if (pclass->properties[i].str_value) {
                    int sidx = add_string();
                    if (sidx >= 0) {
                        strcpy(string_pool[sidx], pclass->properties[i].str_value);
                        current_compiled_class->properties[current_compiled_class->property_count].str_value = string_pool[sidx];
                    }
                }
                current_compiled_class->property_count++;
            }
            for (int i = 0; i < pclass->method_count && current_compiled_class->method_count < MAX_CLASS_METHODS; i++) {
                int midx = current_compiled_class->method_count++;
                strcpy(current_compiled_class->methods[midx].name, pclass->methods[i].name);
                current_compiled_class->methods[midx].body = strdup(pclass->methods[i].body);
            }
        }
    }
    current_compiled_class->constructor_body = NULL;
}

void alwex_class_add_property(const char* name, const char* value_expr) {
    if (!current_compiled_class || current_compiled_class->property_count >= MAX_CLASS_PROPERTIES) return;
    struct ClassProperty* prop = &current_compiled_class->properties[current_compiled_class->property_count++];
    strcpy(prop->name, name);
    struct Value v = evaluate(value_expr);
    if (v.type == 1) {
        prop->str_value = v.str;
        prop->value = 0;
    } else {
        prop->value = v.num;
        prop->str_value = NULL;
    }
}

void alwex_class_add_method(const char* name, const char* body) {
    if (!current_compiled_class || current_compiled_class->method_count >= MAX_CLASS_METHODS) return;
    struct ClassMethod* m = &current_compiled_class->methods[current_compiled_class->method_count++];
    strcpy(m->name, name);
    m->body = strdup(body);
}

void alwex_class_set_constructor(const char* body) {
    if (current_compiled_class) {
        free(current_compiled_class->constructor_body);
        current_compiled_class->constructor_body = strdup(body);
    }
}

void alwex_class_end() {
    current_compiled_class = NULL;
}

void alwex_object_new(const char* obj_name, const char* class_name, const char* params) {
    struct Class* cls = find_class(class_name);
    if (!cls) { printf("Error: class '%s' not found\n", class_name); return; }
    int idx = add_object();
    if (idx < 0) return;
    struct Object* obj = &objects[idx];
    strcpy(obj->name, obj_name);
    strcpy(obj->class_name, class_name);
    // копируем свойства класса в объект
    obj->property_count = cls->property_count;
    for (int i = 0; i < cls->property_count; i++) {
        obj->properties[i] = cls->properties[i];
        if (cls->properties[i].str_value) {
            int sidx = add_string();
            if (sidx >= 0) {
                strcpy(string_pool[sidx], cls->properties[i].str_value);
                obj->properties[i].str_value = string_pool[sidx];
            }
        }
    }
    // вызов конструктора, если есть, с параметрами (упрощённо: аргументы передаются через глобальные arg0-arg4)
    if (cls->constructor_body) {
        // сохраним контекст
        char saved_context[50];
        strcpy(saved_context, g_current_context_object);
        strcpy(g_current_context_object, obj_name);
        // простейший парс параметров
        char* param_ptr = (char*)params;
        char* param_names[] = {"arg0", "arg1", "arg2", "arg3", "arg4"};
        int pi = 0;
        while (*param_ptr && pi < 5) {
            while (my_isspace(*param_ptr)) param_ptr++;
            if (!*param_ptr) break;
            char val[100] = {0};
            if (*param_ptr == '\'' || *param_ptr == '"') {
                char q = *param_ptr++;
                int j = 0;
                while (*param_ptr && *param_ptr != q && j < 99) val[j++] = *param_ptr++;
                if (*param_ptr == q) param_ptr++;
                val[j] = '\0';
                // присвоить строку параметру
                struct Variable* pvar = find_variable(param_names[pi]);
                if (!pvar) { int vi = add_variable(); if (vi>=0) { pvar = &variables[vi]; strcpy(pvar->name, param_names[pi]); } }
                if (pvar) {
                    int si = add_string();
                    if (si >= 0) { strcpy(string_pool[si], val); pvar->str_value = string_pool[si]; pvar->value = 0; }
                }
            } else {
                int j = 0;
                while (*param_ptr && !my_isspace(*param_ptr) && j < 99) val[j++] = *param_ptr++;
                val[j] = '\0';
                struct Variable* pvar = find_variable(param_names[pi]);
                if (!pvar) { int vi = add_variable(); if (vi>=0) { pvar = &variables[vi]; strcpy(pvar->name, param_names[pi]); } }
                if (pvar) {
                    pvar->value = str_to_double(val);
                    pvar->str_value = NULL;
                }
            }
            pi++;
        }
        // выполнить тело конструктора (оно уже в виде исходного кода, но у нас нет execute в рантайме, поэтому конструктор не будет работать через компилятор! 
        // Нужно другой подход: конструктор должен быть скомпилирован в отдельную функцию. 
        // Поэтому для простоты мы не поддерживаем выполнение конструктора в скомпилированных классах, 
        // либо предлагаем генерировать для каждого конструктора отдельную функцию. 
        // Пока пропустим вызов, оставим как есть – конструктор не выполнится.
        strcpy(g_current_context_object, saved_context);
    }
}

void alwex_object_call_method(const char* obj_name, const char* method_name) {
    struct Object* obj = find_object(obj_name);
    if (!obj) { printf("Error: object '%s' not found\n", obj_name); return; }
    struct Class* cls = find_class(obj->class_name);
    if (!cls) { printf("Error: class '%s' not found\n", obj->class_name); return; }
    for (int i = 0; i < cls->method_count; i++) {
        if (strcmp(cls->methods[i].name, method_name) == 0) {
            // выполнить тело метода (аналогично конструктору – не можем)
            printf("Calling method %s::%s (not implemented in compiled code yet)\n", obj->class_name, method_name);
            return;
        }
    }
    printf("Error: method '%s' not found\n", method_name);
}

void alwex_array_create(const char* name) {
    struct Array* arr = find_array(name);
    if (!arr) {
        int idx = add_array();
        if (idx < 0) return;
        arr = &arrays[idx];
        strcpy(arr->name, name);
    }
    arr->size = 0;
    arr->is_string_array = 0; // будет уточняться при добавлении
}

int my_isspace(int c) {
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

void print_double(double n) {
    int integer_part = (int)n;
    double fractional = n - integer_part;
    if (fractional < 0) fractional = -fractional;
    
    int fractional_part = (int)(fractional * 10000);
    printf("%d.%04d", integer_part, fractional_part);
}

void replace_text_operators(char* token) {
    char* op_ptr = token;
    
    while ((op_ptr = strstr(op_ptr, "plus"))) {
        if ((op_ptr == token || !isalnum(*(op_ptr - 1))) && 
            !isalnum(*(op_ptr + 4))) {
            *op_ptr = '+';
            memmove(op_ptr + 1, op_ptr + 4, strlen(op_ptr + 4) + 1);
        } else {
            op_ptr++;
        }
    }
    
    op_ptr = token;
    while ((op_ptr = strstr(op_ptr, "minus"))) {
        if ((op_ptr == token || !isalnum(*(op_ptr - 1))) && 
            !isalnum(*(op_ptr + 5))) {
            *op_ptr = '-';
            memmove(op_ptr + 1, op_ptr + 5, strlen(op_ptr + 5) + 1);
        } else {
            op_ptr++;
        }
    }
    
    op_ptr = token;
    while ((op_ptr = strstr(op_ptr, "mul"))) {
        if ((op_ptr == token || !isalnum(*(op_ptr - 1))) && 
            !isalnum(*(op_ptr + 3))) {
            *op_ptr = '*';
            memmove(op_ptr + 1, op_ptr + 3, strlen(op_ptr + 3) + 1);
        } else {
            op_ptr++;
        }
    }
    
    op_ptr = token;
    while ((op_ptr = strstr(op_ptr, "div"))) {
        if ((op_ptr == token || !isalnum(*(op_ptr - 1))) && 
            !isalnum(*(op_ptr + 3))) {
            *op_ptr = '/';
            memmove(op_ptr + 1, op_ptr + 3, strlen(op_ptr + 3) + 1);
        } else {
            op_ptr++;
        }
    }
    
    op_ptr = token;
    while ((op_ptr = strstr(op_ptr, "inc"))) {
        if ((op_ptr == token || !isalnum(*(op_ptr - 1))) && 
            !isalnum(*(op_ptr + 3))) {
            *op_ptr = '+';
            *(op_ptr + 1) = '+';
            memmove(op_ptr + 2, op_ptr + 3, strlen(op_ptr + 3) + 1);
        } else {
            op_ptr++;
        }
    }
    
    op_ptr = token;
    while ((op_ptr = strstr(op_ptr, "dec"))) {
        if ((op_ptr == token || !isalnum(*(op_ptr - 1))) && 
            !isalnum(*(op_ptr + 3))) {
            *op_ptr = '-';
            *(op_ptr + 1) = '-';
            memmove(op_ptr + 2, op_ptr + 3, strlen(op_ptr + 3) + 1);
        } else {
            op_ptr++;
        }
    }
}

struct Variable* find_variable(const char* name) {
    char clean_name[256] = {0};
    int i = 0;
    while (name[i] && name[i] != '\n' && name[i] != '\r' && name[i] != ' ' && name[i] != '\t' && i < 255) {
        clean_name[i] = name[i];
        i++;
    }
    clean_name[i] = '\0';
    
    for (int j = 0; j < var_count; j++) {
        if (strcmp(variables[j].name, clean_name) == 0) {
            return &variables[j];
        }
    }
    return NULL;
}

struct Function* find_function(const char* name) {
    for (int i = 0; i < function_count; i++) {
        if (strcmp(functions[i].name, name) == 0) {
            return &functions[i];
        }
    }
    return NULL;
}

struct Array* find_array(const char* name) {
    for (int i = 0; i < array_count; i++) {
        if (strcmp(arrays[i].name, name) == 0) {
            return &arrays[i];
        }
    }
    return NULL;
}

struct Class* find_class(const char* name) {
    for (int i = 0; i < class_count; i++) {
        if (strcmp(classes[i].name, name) == 0) {
            return &classes[i];
        }
    }
    return NULL;
}

struct Object* find_object(const char* name) {
    for (int i = 0; i < object_count; i++) {
        if (strcmp(objects[i].name, name) == 0) {
            return &objects[i];
        }
    }
    return NULL;
}

struct Value parse_expression(const char** p);
struct Value parse_term(const char** p);
struct Value parse_factor(const char** p);

void skip_whitespace(const char** p) {
    while (**p && my_isspace(**p)) {
        (*p)++;
    }
}

static void read_identifier(const char** p, char* buf, size_t bufsize) {
    int i = 0;
    while (isalnum(**p) || **p == '_') {
        if (i < (int)bufsize - 1) buf[i++] = **p;
        (*p)++;
    }
    buf[i] = '\0';
}

struct Value parse_factor(const char** p) {
    skip_whitespace(p);
    struct Value result = {0, 0, NULL};
    int negative = 0;

    if (**p == '-') {
        negative = 1;
        (*p)++;
        skip_whitespace(p);
    }

    if (**p == '(') {
        (*p)++;
        result = parse_expression(p);
        skip_whitespace(p);
        if (**p == ')') (*p)++;
        if (negative && result.type == 0) result.num = -result.num;
        return result;
    }

    else if (**p == '\'' || **p == '"') {
        char quote = **p;
        (*p)++;
        char temp[STRING_SIZE] = {0};
        int i = 0;
        while (**p && **p != quote && i < STRING_SIZE - 1) {
            temp[i++] = **p;
            (*p)++;
        }
        if (**p == quote) (*p)++;
        temp[i] = '\0';
        int idx = add_string();
        if (idx >= 0) {
            strcpy(string_pool[idx], temp);
            result.type = 1;
            result.str = string_pool[idx];
            result.num = 0;
        }
        return result;
    }

    else if (isdigit(**p) || **p == '.') {
        result.num = str_to_double(*p);
        while (isdigit(**p) || **p == '.') (*p)++;
        result.type = 0;
        if (negative) result.num = -result.num;
        return result;
    }

    else if (isalpha(**p) || **p == '_') {
        char name[50] = {0};
        read_identifier(p, name, sizeof(name));

        skip_whitespace(p);
        if (**p == '(') {
            (*p)++; 
            struct Value args[10];
            int arg_count = 0;
            while (**p && **p != ')') {
                skip_whitespace(p);
                if (**p == ')') break;
                args[arg_count++] = parse_expression(p);
                skip_whitespace(p);
                if (**p == ',') (*p)++;
            }
            if (**p == ')') (*p)++;

            if (strcmp(name, "len") == 0) {
                if (arg_count == 1 && args[0].type == 1) {
                    result.type = 0;
                    result.num = strlen(args[0].str);
                } else {
                    printf("Error: len() expects one string argument\n");
                    result.type = 0; result.num = 0;
                }
            }
            else if (strcmp(name, "slice") == 0) {
                if (arg_count == 3 && args[0].type == 1 && args[1].type == 0 && args[2].type == 0) {
                    int start = (int)args[1].num;
                    int end   = (int)args[2].num;
                    int len = strlen(args[0].str);
                    if (start < 0) start = 0;
                    if (end > len) end = len;
                    if (start > end) { start = 0; end = 0; }
                    char temp[STRING_SIZE] = {0};
                    strncpy(temp, args[0].str + start, end - start);
                    temp[end - start] = '\0';
                    int idx = add_string();
                    if (idx >= 0) {
                        strcpy(string_pool[idx], temp);
                        result.type = 1;
                        result.str = string_pool[idx];
                    }
                } else {
                    printf("Error: slice(string, start, end) expects string and two numbers\n");
                    result.type = 0; result.num = 0;
                }
            }
            else if (strcmp(name, "contains") == 0) {
                if (arg_count == 2 && args[0].type == 1 && args[1].type == 1) {
                    result.type = 0;
                    result.num = (strstr(args[0].str, args[1].str) != NULL) ? 1 : 0;
                } else {
                    printf("Error: contains(string, substring) expects two strings\n");
                    result.type = 0; result.num = 0;
                }
            }
            else if (strcmp(name, "replace") == 0) {
                if (arg_count == 3 && args[0].type == 1 && args[1].type == 1 && args[2].type == 1) {
                    char* pos = strstr(args[0].str, args[1].str);
                    if (pos) {
                        char temp[STRING_SIZE] = {0};
                        int prefix_len = pos - args[0].str;
                        int old_len = strlen(args[1].str);
                        int new_len = strlen(args[2].str);
                        if (prefix_len + new_len + strlen(pos + old_len) + 1 < STRING_SIZE) {
                            strncpy(temp, args[0].str, prefix_len);
                            temp[prefix_len] = '\0';
                            strcat(temp, args[2].str);
                            strcat(temp, pos + old_len);
                            int idx = add_string();
                            if (idx >= 0) {
                                strcpy(string_pool[idx], temp);
                                result.type = 1;
                                result.str = string_pool[idx];
                            }
                        } else {
                            printf("Error: result of replace is too long\n");
                            result.type = 0; result.num = 0;
                        }
                    } else {
                        int idx = add_string();
                        if (idx >= 0) {
                            strcpy(string_pool[idx], args[0].str);
                            result.type = 1;
                            result.str = string_pool[idx];
                        }
                    }
                } else {
                    printf("Error: replace(string, old, new) expects three strings\n");
                    result.type = 0; result.num = 0;
                }
            }
            else {
                printf("Error: unknown function '%s'\n", name);
                result.type = 0; result.num = 0;
            }
            if (negative && result.type == 0) result.num = -result.num;
            return result;
        }

        if (strcmp(name, "__rand_internal") == 0) {
            result.type = 0;
            result.num = (double)alwex_rand();
        } else {
            struct Variable* v = find_variable(name);
            if (v) {
                if (v->str_value) {
                    result.type = 1;
                    result.str = v->str_value;
                    result.num = 0;
                } else {
                    result.type = 0;
                    result.num = v->value;
                }
            } else {
                result.type = 0;
                result.num = 0;
            }
        }
        if (negative && result.type == 0) result.num = -result.num;
        return result;
    }

    result.type = 0;
    result.num = 0;
    return result;
}

struct Value parse_term(const char** p) {
    struct Value left = parse_factor(p);
    while (1) {
        skip_whitespace(p);
        if (**p == '*') {
            (*p)++;
            struct Value right = parse_factor(p);
            if (left.type == 0 && right.type == 0) {
                left.num *= right.num;
            } else {
                printf("Error: * requires numeric operands\n");
                left.type = 0; left.num = 0;
            }
        }
        else if (**p == '/') {
            (*p)++;
            struct Value right = parse_factor(p);
            if (left.type == 0 && right.type == 0) {
                if (right.num != 0) left.num /= right.num;
                else printf("Error: division by zero\n");
            } else {
                printf("Error: / requires numeric operands\n");
                left.type = 0; left.num = 0;
            }
        }
        else if (**p == '%') {
            (*p)++;
            struct Value right = parse_factor(p);
            if (left.type == 0 && right.type == 0) {
                if (right.num != 0) left.num = (int)left.num % (int)right.num;
                else printf("Error: modulo by zero\n");
            } else {
                printf("Error: %% requires numeric operands\n");
                left.type = 0; left.num = 0;
            }
        }
        else break;
    }
    return left;
}

struct Value parse_expression(const char** p) {
    struct Value left = parse_term(p);
    while (1) {
        skip_whitespace(p);
        if (**p == '+') {
            (*p)++;
            struct Value right = parse_term(p);
            if (left.type == 0 && right.type == 0) {
                left.num += right.num;
            }
            else if (left.type == 1 && right.type == 1) {
                char temp[STRING_SIZE * 2] = {0};
                strcpy(temp, left.str);
                strcat(temp, right.str);
                int idx = add_string();
                if (idx >= 0) {
                    strcpy(string_pool[idx], temp);
                    left.str = string_pool[idx];
                } else {
                    printf("Error: string pool full\n");
                    left.type = 0; left.num = 0;
                }
            }
            else {
                // ОДИН ИЗ ОПЕРАНДОВ СТРОКА, ДРУГОЙ ЧИСЛО – ПРЕОБРАЗУЕМ ЧИСЛО В СТРОКУ
                char num_str[64];
                if (left.type == 1 && right.type == 0) {
                    alwex_str_from_number(right.num, num_str, sizeof(num_str));
                    right.str = num_str;
                    right.type = 1;
                } else if (left.type == 0 && right.type == 1) {
                    alwex_str_from_number(left.num, num_str, sizeof(num_str));
                    left.str = num_str;
                    left.type = 1;
                }
                // теперь обе строки
                char temp[STRING_SIZE * 2] = {0};
                strcpy(temp, left.str);
                strcat(temp, right.str);
                int idx = add_string();
                if (idx >= 0) {
                    strcpy(string_pool[idx], temp);
                    left.str = string_pool[idx];
                }
            }
        }
        else if (**p == '-') {
            (*p)++;
            struct Value right = parse_term(p);
            if (left.type == 0 && right.type == 0) {
                left.num -= right.num;
            } else {
                printf("Error: - requires numeric operands\n");
                left.type = 0; left.num = 0;
            }
        }
        else break;
    }
    return left;
}

struct Value evaluate(const char* expr) {
    const char* p = expr;
    return parse_expression(&p);
}

double eval_expression(const char* expr) {
    struct Value v = evaluate(expr);
    return v.type == 0 ? v.num : 0;
}


int eval_condition(const char* cond) {
    const char* operators[] = {"==", "!=", ">=", "<=", ">", "<"};
    int op_count = 6;
    
    for (int i = 0; i < op_count; i++) {
        const char* op_pos = strstr(cond, operators[i]);
        if (op_pos) {
            char left[100] = {0};
            char right[100] = {0};
            
            int left_len = op_pos - cond;
            if (left_len >= 100) left_len = 99;
            strncpy(left, cond, left_len);
            left[left_len] = '\0';
            
            strcpy(right, op_pos + strlen(operators[i]));

            char* ltrim = left;
            while (my_isspace(*ltrim)) ltrim++;
            char* lend = left + strlen(left) - 1;
            while (lend > left && my_isspace(*lend)) *lend-- = '\0';
            
            char* rtrim = right;
            while (my_isspace(*rtrim)) rtrim++;
            char* rend = right + strlen(right) - 1;
            while (rend > right && my_isspace(*rend)) *rend-- = '\0';

            int left_is_string = (*ltrim == '\'' || *ltrim == '"');
            int right_is_string = (*rtrim == '\'' || *rtrim == '"');
            // проверяем строковые переменные
            if (!left_is_string) {
                struct Variable* v = find_variable(ltrim);
                if (v && v->str_value) left_is_string = 1;
            }
            if (!right_is_string) {
                struct Variable* v = find_variable(rtrim);
                if (v && v->str_value) right_is_string = 1;
            }
            
            if (left_is_string || right_is_string) {
                char left_str[100] = {0};
                if (left_is_string) {
                    if (*ltrim == '\'' || *ltrim == '"') {
                        char quote = *ltrim;
                        strcpy(left_str, ltrim + 1);
                        char* qe = strchr(left_str, quote);
                        if (qe) *qe = '\0';
                    } else {
                        struct Variable* v = find_variable(ltrim);
                        if (v && v->str_value) strcpy(left_str, v->str_value);
                    }
                } else {
                    struct Variable* v = find_variable(ltrim);
                    if (v && v->str_value) strcpy(left_str, v->str_value);
                    else if (v) snprintf(left_str, sizeof(left_str), "%.0f", v->value);
                }

                char right_str[100] = {0};
                if (right_is_string) {
                    if (*rtrim == '\'' || *rtrim == '"') {
                        char quote = *rtrim;
                        strcpy(right_str, rtrim + 1);
                        char* qe = strchr(right_str, quote);
                        if (qe) *qe = '\0';
                    } else {
                        struct Variable* v = find_variable(rtrim);
                        if (v && v->str_value) strcpy(right_str, v->str_value);
                    }
                } else {
                    struct Variable* v = find_variable(rtrim);
                    if (v && v->str_value) strcpy(right_str, v->str_value);
                    else if (v) snprintf(right_str, sizeof(right_str), "%.0f", v->value);
                }

                int cmp = strcmp(left_str, right_str);
                
                if (strcmp(operators[i], "==") == 0) return cmp == 0;
                if (strcmp(operators[i], "!=") == 0) return cmp != 0;
                return 0;
            } else {
                double left_val = 0;
                double right_val = 0;

                if (isdigit(*ltrim) || *ltrim == '-') {
                    left_val = str_to_double(ltrim);
                } else {
                    struct Variable* v = find_variable(ltrim);
                    if (v) left_val = v->value;
                }

                if (isdigit(*rtrim) || *rtrim == '-') {
                    right_val = str_to_double(rtrim);
                } else {
                    struct Variable* v = find_variable(rtrim);
                    if (v) right_val = v->value;
                }
                
                if (strcmp(operators[i], "==") == 0) return left_val == right_val;
                if (strcmp(operators[i], "!=") == 0) return left_val != right_val;
                if (strcmp(operators[i], ">=") == 0) return left_val >= right_val;
                if (strcmp(operators[i], "<=") == 0) return left_val <= right_val;
                if (strcmp(operators[i], ">") == 0) return left_val > right_val;
                if (strcmp(operators[i], "<") == 0) return left_val < right_val;
            }
        }
    }
    
    return 0;
}

void import_library(const char* libname, int import_depth);

void init_memory() {
    var_capacity = 10;
    variables = malloc(var_capacity * sizeof(struct Variable));
    
    string_capacity = 10;
    string_pool = malloc(string_capacity * sizeof(char*));
    for (int i = 0; i < string_capacity; i++) {
        string_pool[i] = malloc(STRING_SIZE);
    }
    
    function_capacity = 10;
    functions = malloc(function_capacity * sizeof(struct Function));

    array_capacity = 10;
    arrays = calloc(array_capacity, sizeof(struct Array));

    class_capacity = 10;
    classes = calloc(class_capacity, sizeof(struct Class));
    
    object_capacity = 10;
    objects = calloc(object_capacity, sizeof(struct Object));
}

void free_memory() {
    free(variables);
    
    for (int i = 0; i < string_capacity; i++) {
        free(string_pool[i]);
    }
    free(string_pool);
    
    for (int i = 0; i < function_count; i++) {
        free(functions[i].body);
    }
    free(functions);

    if (arrays) {
        free(arrays);
    }
    
    if (classes) {
        for (int i = 0; i < class_count; i++) {
            for (int j = 0; j < classes[i].method_count; j++) {
                if (classes[i].methods[j].body) {
                    free(classes[i].methods[j].body);
                }
            }
            if (classes[i].constructor_body) {
                free(classes[i].constructor_body);
            }
        }
        free(classes);
    }
    
    if (objects) {
        free(objects);
    }
    
    if (last_http_response.data) {
        free(last_http_response.data);
    }
}

int add_variable() {
    if (var_count >= var_capacity) {
        var_capacity *= 2;
        variables = realloc(variables, var_capacity * sizeof(struct Variable));
        if (!variables) {
            printf("Error: memory allocation failed for variables\n");
            return -1;
        }
    }
    return var_count++;
}

int add_string() {
    if (string_count >= string_capacity) {
        int new_capacity = string_capacity * 2;
        char **new_pool = realloc(string_pool, new_capacity * sizeof(char*));
        if (!new_pool) {
            printf("Error: memory allocation failed for string pool\n");
            return -1;
        }
        
        for (int i = string_capacity; i < new_capacity; i++) {
            new_pool[i] = malloc(STRING_SIZE);
            if (!new_pool[i]) {
                printf("Error: memory allocation failed for string %d\n", i);
                return -1;
            }
        }
        
        string_pool = new_pool;
        string_capacity = new_capacity;
    }
    return string_count++;
}

int add_function() {
    if (function_count >= function_capacity) {
        function_capacity *= 2;
        functions = realloc(functions, function_capacity * sizeof(struct Function));
        if (!functions) {
            printf("Error: memory allocation failed for functions\n");
            return -1;
        }
    }
    return function_count++;
}

int add_array() {
    if (array_count >= array_capacity) {
        array_capacity *= 2;
        arrays = realloc(arrays, array_capacity * sizeof(struct Array));
    }
    return array_count++;
}

static void clean_var_name(const char* src, char* dst, int dst_size) {
    int i = 0;

    while (*src && isspace((unsigned char)*src)) {
        src++;
    }

    while (*src && i < dst_size - 1) {
        char c = *src;
        if ( (c >= 'a' && c <= 'z') ||
             (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9') ||
             c == '_' ) {
            dst[i++] = c;
            src++;
        } else {
            break;
        }
    }
    dst[i] = '\0';
}

int add_class() {
    if (class_count >= class_capacity) {
        class_capacity *= 2;
        classes = realloc(classes, class_capacity * sizeof(struct Class));
        if (!classes) {
            printf("Error: memory allocation failed for classes\n");
            return -1;
        }
    }
    int idx = class_count++;
    classes[idx].parent_name[0] = '\0';
    classes[idx].property_count = 0;
    classes[idx].method_count = 0;
    classes[idx].constructor_body = NULL;
    return idx;
}

int add_object() {
    if (object_count >= object_capacity) {
        object_capacity *= 2;
        objects = realloc(objects, object_capacity * sizeof(struct Object));
        if (!objects) {
            printf("Error: memory allocation failed for objects\n");
            return -1;
        }
    }
    return object_count++;
}

int file_exists(const char* path) {
    FILE* f = fopen(path, "r");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

int dir_exists(const char* path) {
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

void create_directory(const char* path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

void parse_version(const char* json, char* version, size_t max_len) {
    const char* ver_start = strstr(json, "\"version\"");
    if (!ver_start) {
        version[0] = '\0';
        return;
    }
    
    ver_start = strchr(ver_start, ':');
    if (!ver_start) {
        version[0] = '\0';
        return;
    }
    
    ver_start = strchr(ver_start, '"');
    if (!ver_start) {
        version[0] = '\0';
        return;
    }
    ver_start++;
    
    const char* ver_end = strchr(ver_start, '"');
    if (!ver_end) {
        version[0] = '\0';
        return;
    }
    
    size_t len = ver_end - ver_start;
    if (len >= max_len) len = max_len - 1;
    
    strncpy(version, ver_start, len);
    version[len] = '\0';
}

int compare_versions(const char* v1, const char* v2) {
    int major1 = 0, minor1 = 0, patch1 = 0;
    int major2 = 0, minor2 = 0, patch2 = 0;
    
    sscanf(v1, "%d.%d.%d", &major1, &minor1, &patch1);
    sscanf(v2, "%d.%d.%d", &major2, &minor2, &patch2);
    
    if (major1 != major2) return major1 > major2 ? 1 : -1;
    if (minor1 != minor2) return minor1 > minor2 ? 1 : -1;
    if (patch1 != patch2) return patch1 > patch2 ? 1 : -1;
    
    return 0;
}


void install_package(const char* package_name) {
    printf("Installing library '%s'...\n", package_name);
    
    char base_url[2048];
    char json_url[2048];
    char alw_url[2048];
    char local_lib_dir[512];
    char local_json_path[512];
    char local_alw_path[512];

    snprintf(base_url, sizeof(base_url), 
             "https://raw.githubusercontent.com/alwex0920/alwexscript-package/main/%s", 
             package_name);
    snprintf(json_url, sizeof(json_url), "%s/alwex.json", base_url);
    snprintf(alw_url, sizeof(alw_url), "%s/%s.alw", base_url, package_name);

    snprintf(local_lib_dir, sizeof(local_lib_dir), "%s/lib/%s", interpreter_dir, package_name);
    snprintf(local_json_path, sizeof(local_json_path), "%s/lib/%s/alwex.json", interpreter_dir, package_name);
    snprintf(local_alw_path, sizeof(local_alw_path), "%s/lib/%s/%s.alw", interpreter_dir, package_name, package_name);

    char lib_base[512];
    snprintf(lib_base, sizeof(lib_base), "%s/lib", interpreter_dir);

    printf("Downloading metadata...\n");
    http_get(json_url);
    
    if (!last_http_response.data || last_http_response.size == 0) {
        printf("Error: couldn't download metadata for '%s'\n", package_name);
        printf("Check that the library exists in the repository:\n");
        printf("  %s\n", json_url);
        return;
    }

    char remote_version[32];
    parse_version(last_http_response.data, remote_version, sizeof(remote_version));
    
    if (remote_version[0] == '\0') {
        printf("Error: couldn't read the version from alwex.json\n");
        return;
    }
    
    printf("Version in the repository: %s\n", remote_version);

    if (file_exists(local_json_path)) {
        FILE* local_json_file = fopen(local_json_path, "r");
        if (local_json_file) {
            fseek(local_json_file, 0, SEEK_END);
            long local_size = ftell(local_json_file);
            fseek(local_json_file, 0, SEEK_SET);
            
            char* local_json_data = malloc(local_size + 1);
            if (local_json_data) {
                fread(local_json_data, 1, local_size, local_json_file);
                local_json_data[local_size] = '\0';
                
                char local_version[32];
                parse_version(local_json_data, local_version, sizeof(local_version));
                
                printf("Installed version: %s\n", local_version);
                
                int cmp = compare_versions(remote_version, local_version);
                
                if (cmp < 0) {
                    printf("You have a newer version (%s) installed. No update is required.\n", local_version);
                    free(local_json_data);
                    fclose(local_json_file);
                    return;
                } else if (cmp == 0) {
                    printf("The %s library is already installed (%s version)\n", package_name, local_version);
                    free(local_json_data);
                    fclose(local_json_file);
                    return;
                } else {
                    printf("An update is available: %s -> %s\n", local_version, remote_version);
                }
                
                free(local_json_data);
            }
            fclose(local_json_file);
        }
    }

    if (!dir_exists(lib_base)) {
        create_directory(lib_base);
    }

    if (!dir_exists(local_lib_dir)) {
        create_directory(local_lib_dir);
    }

    printf("Saving alwex.json...\n");
    FILE* json_file = fopen(local_json_path, "w");
    if (!json_file) {
        printf("Error: failed to create a file %s\n", local_json_path);
        return;
    }
    fwrite(last_http_response.data, 1, last_http_response.size, json_file);
    fclose(json_file);

    printf("Loading %s.alw...\n", package_name);
    http_download(alw_url, local_alw_path);
    
    printf("✓ %s library has been successfully installed (%s version)\n", package_name, remote_version);
}

void import_library(const char* libname, int import_depth) {
    if (import_depth > MAX_IMPORT_DEPTH) {
        printf("Error: import depth too deep for library '%s'\n", libname);
        return;
    }
    
    char lib_dir[512];
    char lib_path[512];
    char json_path[512];
    char local_path[1024];

    snprintf(lib_dir,   sizeof(lib_dir),   "%s/lib/%s", interpreter_dir, libname);
    snprintf(lib_path,  sizeof(lib_path),  "%s/lib/%s/%s.alw", interpreter_dir, libname, libname);
    snprintf(json_path, sizeof(json_path), "%s/lib/%s/alwex.json", interpreter_dir, libname);

    if (dir_exists(lib_dir)) {
        if (file_exists(lib_path) && file_exists(json_path)) {
            FILE* file = fopen(lib_path, "r");
            if (!file) {
                printf("Error: couldn't open library '%s'\n", libname);
                return;
            }

            fseek(file, 0, SEEK_END);
            long size = ftell(file);
            fseek(file, 0, SEEK_SET);

            char* code = malloc(size + 1);
            if (!code) {
                fclose(file);
                printf("Error: there is not enough memory to load the library. '%s'\n", libname);
                return;
            }

            fread(code, 1, size, file);
            code[size] = '\0';
            fclose(file);

            /* compiled runtime: library code loaded but not executed */
            free(code);
            return;

        } else if (file_exists(json_path) && !file_exists(lib_path)) {
            printf("Error: The '%s' library is damaged! The %s.alw file is missing. A reinstall is required.\n", libname, libname);
            return;

        } else {
            printf("Error: the '%s' library is damaged or not fully installed.\n", libname);
            return;
        }
    }

    extern char current_script_dir[512];

    if (current_script_dir[0] != '\0') {
        snprintf(local_path, sizeof(local_path), "%s/%s.alw", current_script_dir, libname);
        
        if (file_exists(local_path)) {
            FILE* file = fopen(local_path, "r");
            if (!file) {
                printf("Error: couldn't open the local library '%s'\n", libname);
                return;
            }

            fseek(file, 0, SEEK_END);
            long size = ftell(file);
            fseek(file, 0, SEEK_SET);

            char* code = malloc(size + 1);
            if (!code) {
                fclose(file);
                printf("Error: not enough memory\n");
                return;
            }

            fread(code, 1, size, file);
            code[size] = '\0';
            fclose(file);

            /* compiled runtime: library code loaded but not executed */
            free(code);
            return;
        }
    }

    printf("Error: The %s library was not found either in lib/ or next to the script\n", libname);
}

// ---------- обёртки для сгенерированного C-кода ----------
void var_assign(const char* name, const char* expr) {
    struct Value v = evaluate(expr);
    struct Variable* var = find_variable(name);
    if (!var) {
        int idx = add_variable();
        if (idx < 0) return;
        var = &variables[idx];
        strncpy(var->name, name, 49);
        var->name[49] = '\0';
    }
    if (v.type == 1) {
        var->str_value = v.str;
        var->value = 0;
    } else {
        var->value = v.num;
        var->str_value = NULL;
    }
}

void var_print(const char* name) {
    struct Variable* v = find_variable(name);
    if (!v) { printf("null\n"); return; }
    if (v->str_value) printf("%s\n", v->str_value);
    else { print_double(v->value); printf("\n"); }
}

void array_print(const char* arr_name, int index) {
    struct Array* arr = find_array(arr_name);
    if (!arr || index < 0 || index >= arr->size) {
        printf("Error: array index out of bounds\n");
        return;
    }
    if (arr->is_string_array) printf("%s\n", arr->strings[index]);
    else { print_double(arr->values[index]); printf("\n"); }
}

void object_print_property(const char* obj_name, const char* prop_name) {
    struct Object* obj = find_object(obj_name);
    if (!obj) { printf("Error: object '%s' not found\n", obj_name); return; }
    for (int i = 0; i < obj->property_count; i++) {
        if (strcmp(obj->properties[i].name, prop_name) == 0) {
            if (obj->properties[i].str_value)
                printf("%s\n", obj->properties[i].str_value);
            else { print_double(obj->properties[i].value); printf("\n"); }
            return;
        }
    }
    printf("Error: property '%s' not found in object '%s'\n", prop_name, obj_name);
}

void object_set_property(const char* obj_name, const char* prop_name, const char* expr) {
    struct Object* obj = find_object(obj_name);
    if (!obj) { printf("Error: object '%s' not found\n", obj_name); return; }
    struct Value v = evaluate(expr);
    for (int i = 0; i < obj->property_count; i++) {
        if (strcmp(obj->properties[i].name, prop_name) == 0) {
            if (v.type == 1) {
                obj->properties[i].str_value = v.str;
                obj->properties[i].value = 0;
            } else {
                obj->properties[i].value = v.num;
                obj->properties[i].str_value = NULL;
            }
            return;
        }
    }
    if (obj->property_count < MAX_CLASS_PROPERTIES) {
        struct ClassProperty* p = &obj->properties[obj->property_count++];
        strncpy(p->name, prop_name, 49);
        p->name[49] = '\0';
        if (v.type == 1) { p->str_value = v.str; p->value = 0; }
        else { p->value = v.num; p->str_value = NULL; }
    }
}

void import_library_wrapper(const char* libname) {
    import_library(libname, 0);
}