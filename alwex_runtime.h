#ifndef ALWEX_RUNTIME_H
#define ALWEX_RUNTIME_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
    #include <windows.h>
    #include <wininet.h>
    #pragma comment(lib, "wininet.lib")
    #define sleep(seconds) Sleep(seconds * 1000)
#else
    #include <unistd.h>
    #include <curl/curl.h>
#endif

// ---------- константы ----------
#define STRING_SIZE 1024
#define MAX_VARS 10000
#define MAX_STRINGS 1000
#define MAX_FUNCTIONS 500
#define MAX_ARRAY_SIZE 1000
#define MAX_ARRAYS 100
#define MAX_CLASSES 100
#define MAX_CLASS_PROPERTIES 50
#define MAX_CLASS_METHODS 50
#define MAX_OBJECTS 500
#define MAX_IMPORT_DEPTH 50

// ---------- структуры данных ----------
struct Variable {
    char name[50];
    double value;
    char* str_value;
};

struct Array {
    char name[50];
    double values[MAX_ARRAY_SIZE];
    char* strings[MAX_ARRAY_SIZE];
    int size;
    int is_string_array;
};

struct Function {
    char name[50];
    char* body;
};

struct ClassProperty {
    char name[50];
    double value;
    char* str_value;
};

struct ClassMethod {
    char name[50];
    char* body;
};

struct Class {
    char name[50];
    char parent_name[50];
    struct ClassProperty properties[MAX_CLASS_PROPERTIES];
    int property_count;
    struct ClassMethod methods[MAX_CLASS_METHODS];
    int method_count;
    char* constructor_body;
};

struct Object {
    char name[50];
    char class_name[50];
    struct ClassProperty properties[MAX_CLASS_PROPERTIES];
    int property_count;
};

struct Value {
    int type;   // 0 = число, 1 = строка
    double num;
    char* str;
};

struct HttpResponse {
    char* data;
    size_t size;
};

// ---------- глобальные переменные рантайма ----------
extern struct Variable* variables;
extern int var_count;
extern char** string_pool;
extern int string_count;
extern struct Array* arrays;
extern int array_count;
extern struct Function* functions;
extern int function_count;
extern struct Class* classes;
extern int class_count;
extern struct Object* objects;
extern int object_count;
extern struct HttpResponse last_http_response;
extern char current_script_dir[512];
extern char interpreter_dir[512];

// ---------- функции рантайма ----------
void init_memory();
void free_memory();

int add_variable();
int add_string();
int add_array();
int add_function();
int add_class();
int add_object();

struct Variable* find_variable(const char* name);
struct Array* find_array(const char* name);
struct Function* find_function(const char* name);
struct Class* find_class(const char* name);
struct Object* find_object(const char* name);

double str_to_double(const char* s);
void print_double(double n);
struct Value evaluate(const char* expr);
int eval_condition(const char* cond);
int my_isspace(int c);

void http_get(const char* url);
void http_post(const char* url, const char* data);
void http_download(const char* url, const char* filename);

void alwex_srand(unsigned int seed);
int alwex_rand();
void alwex_input_string(const char* prompt, const char* var_name);
void alwex_input_number(const char* prompt, const char* var_name);
void alwex_str_from_number(double num, char* out, size_t out_size);
void alwex_arr_get(const char* arr_name, int index);
void alwex_arr_set(const char* arr_name, int index, double value);
void alwex_arr_push_number(const char* arr_name, double value);
void alwex_arr_push_string(const char* arr_name, const char* value);
void alwex_arr_length(const char* arr_name);
void alwex_array_create(const char* name);
void alwex_arr_push_string(const char* arr_name, const char* value);
void alwex_arr_push_number(const char* arr_name, double value);
void alwex_file_write(const char* filename, const char* content);
void alwex_file_read(const char* filename);
void alwex_file_append(const char* filename, const char* content);
void alwex_file_exists(const char* filename);
void alwex_inc_var(const char* name);
void alwex_dec_var(const char* name);
void alwex_str_split(const char* var_name, const char* delim, const char* arr_name);
void alwex_object_call_method(const char* obj_name, const char* method_name);
void alwex_class_begin(const char* name, const char* parent);
void alwex_class_add_property(const char* name, const char* value_expr);
void alwex_class_add_method(const char* name, const char* body);
void alwex_class_set_constructor(const char* body);
void alwex_class_end();
void alwex_object_new(const char* obj_name, const char* class_name, const char* params);
void alwex_object_call_method(const char* obj_name, const char* method_name);

// вспомогательные функции для сгенерированного кода
void var_assign(const char* name, const char* expr);
void var_print(const char* name);
void array_print(const char* arr_name, int index);
void import_library(const char* libname, int depth);
void execute_function(const char* name);
void object_set_property(const char* obj_name, const char* prop_name, const char* expr);
void object_print_property(const char* obj_name, const char* prop_name);

#endif