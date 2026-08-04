Для компилирования своего скрипта используйте команду
```bash
alwex_compiler <ваш_скрипт.alw>
```
Для компиляции компилятора используйте эти команды
```bash
gcc -c alwex_runtime.c -o alwex_runtime.o
gcc alwex_compiler.c -o alwex_compiler
```

P.S: Версия компилятора желательно должна совпадать с версией AlwexScript который у вас установлен
