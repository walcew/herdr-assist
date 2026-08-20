"""Define LV_CONF_PATH com barras normais (/), inclusive no Windows.

Antes isto vinha de `-D LV_CONF_PATH=${PROJECT_DIR}/src/lv_conf.h` no
platformio.ini. No Windows o ${PROJECT_DIR} traz separadores "\\" que o SCons
consome ao montar o argumento -D, e o LVGL acaba tentando abrir um caminho sem
as barras ("C:Usersbruno...lv_conf.h") — o build inteiro quebra no include do
lv_conf.h. No Linux/CI não há "\\", então o replace é no-op e o comportamento é
idêntico ao anterior.

Registrado como `extra_scripts = pre:scripts/pio_lvconf.py`.
"""
Import("env")  # noqa: F821  (injetado pelo PlatformIO/SCons)

project_dir = env.subst("$PROJECT_DIR").replace("\\", "/")
env.Append(CPPDEFINES=[("LV_CONF_PATH", project_dir + "/src/lv_conf.h")])
