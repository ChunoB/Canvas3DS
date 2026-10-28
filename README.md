# Canvas 3DS

Cliente do [Canvas LMS](https://www.instructure.com/canvas) para Nintendo 3DS (Old e New), feito para o Canvas do Einstein
(`einstein.instructure.com`), mas funciona com qualquer Canvas.

Mostra cursos, tarefas com prazos, avisos, módulos, páginas, discussões, arquivos, notas e a caixa de entrada (com resposta
pelo teclado do 3DS). Usa a API REST oficial do Canvas com um token de acesso pessoal. O conteúdo em HTML é mostrado por um
leitor leve próprio (`source/html.c`), sem JavaScript.

Instalação e uso: veja [LEIA-ME.txt](LEIA-ME.txt). Baixe o pacote mais novo em
[Releases](https://github.com/ChunoB/Canvas3DS/releases/latest).

## Compilar

Precisa do devkitPro com `3ds-dev`, `3ds-curl`, `3ds-mbedtls` e `3ds-zlib`. `make` gera o `.3dsx`; `make cia` gera o `.cia`
usando `tools/bin/makerom.exe` ([Project_CTR](https://github.com/3DSGuy/Project_CTR/releases)) e `tools/bin/bannertool.exe`
([bannertool](https://github.com/diasurgical/bannertool/releases)), que não vão no repositório.

Versão nova: aumente `VERSION_MAJOR/MINOR/MICRO` no `Makefile`, rode `make cia` e `python tools/package.py . Canvas3DS`, e
publique uma release `vX.Y.Z` com o `.3dsx`, o `.cia` e o `dist/Canvas3DS-pacote.zip`. O app se atualiza sozinho pela release.

## Arquivos

| Arquivo | O que faz |
|---|---|
| `source/main.c` | Telas, toque e botões |
| `source/views.c` | Cada tela: pede os dados à API e monta listas e documentos |
| `source/net.c` | Rede em segundo plano (curl), paginação do Canvas, downloads |
| `source/html.c` | Leitor de HTML leve (títulos, listas, tabelas, links) |
| `source/json.c` | Leitor de JSON |
| `source/updater.c` | Atualização pelo próprio app (GitHub releases) |
| `pc/configurar_token.bat` | Confere o token e salva no cartão SD |
