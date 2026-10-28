"""Configura o Canvas 3DS: confere o token de acesso e salva no cartao SD do 3DS.

Como pegar o token: entre no Canvas pelo navegador > Conta > Configuracoes >
"+ Novo token de acesso" > Finalidade "3DS" > Gerar token > copie.
Precisa so do Python 3 (sem bibliotecas extras).
"""
import getpass
import json
import os
import string
import sys
import urllib.error
import urllib.request

PADRAO = "einstein.instructure.com"


def limpar_dominio(d):
    d = d.strip()
    if "://" in d:
        d = d.split("://", 1)[1]
    return d.split("/", 1)[0] or PADRAO


def conferir(dominio, token):
    req = urllib.request.Request(f"https://{dominio}/api/v1/users/self",
                                 headers={"Authorization": f"Bearer {token}", "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def achar_sd():
    achados = []
    for letra in string.ascii_uppercase:
        raiz = f"{letra}:\\"
        if letra == "C" or not os.path.isdir(raiz):
            continue
        if os.path.isdir(os.path.join(raiz, "3ds")) or os.path.isdir(os.path.join(raiz, "Nintendo 3DS")):
            achados.append(raiz)
    return achados


def main():
    print("=== Configurar o Canvas 3DS ===\n")
    d = input(f"Endereco do Canvas [{PADRAO}]: ").strip()
    dominio = limpar_dominio(d) if d else PADRAO
    print("\nCole o token de acesso (ele nao aparece na tela enquanto voce cola) e aperte Enter.")
    token = getpass.getpass("Token: ").strip()
    if not token:
        print("Nenhum token digitado.")
        return 1
    try:
        eu = conferir(dominio, token)
        print(f"\nToken OK! Conta: {eu.get('name', '?')}")
    except urllib.error.HTTPError as e:
        print(f"\nO Canvas recusou o token (erro {e.code}). Gere um novo e tente de novo.")
        return 1
    except Exception as e:  # noqa: BLE001
        print(f"\nNao consegui falar com {dominio}: {e}")
        return 1

    sds = achar_sd()
    if not sds:
        print("\nNao achei o cartao SD do 3DS conectado (uma unidade com a pasta 3ds).")
        print("Coloque o cartao no PC e rode de novo.")
        return 1
    for sd in sds:
        pasta = os.path.join(sd, "3ds", "Canvas3DS")
        os.makedirs(pasta, exist_ok=True)
        with open(os.path.join(pasta, "token.txt"), "w", encoding="utf-8") as f:
            f.write(token + "\n")
        with open(os.path.join(pasta, "config.ini"), "w", encoding="utf-8") as f:
            f.write(f"domain={dominio}\ntz=-180\ntext_size=1\n")
        print(f"Salvo em {pasta}")
    print("\nPronto! Coloque o cartao de volta no 3DS e abra o Canvas 3DS.")
    print("O token fica so no cartao SD. Se perder o 3DS, apague o token no site do Canvas.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
