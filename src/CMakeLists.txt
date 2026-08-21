# Só .c: o glob antigo era `src/*.*` e passaria a engolir o clawd.hav embutido
# abaixo como se fosse código-fonte. Cabeçalhos não precisam estar em SRCS.
FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)

# Avatar de fábrica: o pacote entra no binário e o motor o lê no lugar, sem
# copiar para a PSRAM (ver avatar.c). Gerado por scripts/hav_pack.py.
idf_component_register(SRCS ${app_sources}
                       EMBED_FILES "assets/clawd.hav")
