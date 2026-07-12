#ifndef MODULE_H
#define MODULE_H

typedef void (*module_init_fn)(void);

typedef struct {
    module_init_fn init;
} module_desc_t;

// #define MODULE_REGISTER(init_func) \
//     static const module_desc_t __module_##init_func \
//         __attribute__((used, section("init_modules"))) = { init_func }
/* 注册宏：用于在注册表文件中声明模块 */
#define REGISTER_MODULE(init_func)  { (init_func) }



void modules_init(void);



#endif			




				