# Patches

## `herdr-remote-focus.patch`

O protocolo do relay do [herdr-remote](https://github.com/dcolinmorgan/herdr-remote) não
tem como focar um agente — dá para ler o terminal e mandar teclas, mas não para trazer a
sessão à frente no Mac. Num painel físico esse é justamente o gesto mais natural: ver que
um agente precisa de atenção e mandar a janela dele para a frente.

O patch adiciona o tipo `focus` ao relay, seguindo o mesmo padrão dos comandos existentes
(valida o `pane_id`, registra no log de auditoria, executa `herdr agent focus <pane_id>`).

Aplicar sobre um clone do herdr-remote:

```bash
git apply /caminho/para/herdr-remote-focus.patch
```

Candidato a PR upstream — enquanto não for enviado, o firmware degrada de forma limpa se o
relay não tiver o patch: o relay responde `{"type":"error","message":"..."}` e o botão de
foco simplesmente não faz nada.
