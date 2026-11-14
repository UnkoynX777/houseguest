<p align="center">
  <img src="../../assets/banner.svg" alt="Houseguest: empreste seu PC sem emprestar sua vida" width="100%">
</p>

[English](../../README.md) · Português

**Houseguest é um bloqueador de aplicativos open-source para sessões de convidado no Windows.** Empreste seu PC a um amigo, a uma criança ou a um colega e escolha exatamente quais programas a conta deles pode executar, pelo tempo que você decidir. Quando o tempo acaba, as restrições são removidas e o convidado é desconectado.

Um serviço do Windows aplica as regras, então fechar a janela não muda nada. O código é público, cada bloqueio e cada alteração vai para um registro à prova de adulteração, e os limites do que o Windows consegue garantir estão escritos, não escondidos.

<p align="center">
  <img src="../images/dashboard-pt-BR.png" alt="Painel com uma sessão de convidado ativa" width="80%">
</p>

## Por que o Houseguest

O Windows tem controle parental e recursos de política, mas nada simples para o caso comum: *"vou emprestar meu PC por uma hora; deixe usar o navegador e mais nada"*.

- **Sessões de convidado com cronômetro.** Escolha um perfil, a conta do convidado e a duração. Estenda, pare ou deixe expirar.
- **Controle de acesso a aplicativos por caminho, hash ou fabricante.** Regras de hash e fabricante sobrevivem à renomeação.
- **Perfis prontos** (Convidado, Jogos, Trabalho, Restrito), editáveis, importáveis e exportáveis.
- **Prévia antes de iniciar:** veja quais aplicativos seriam permitidos ou bloqueados.
- **Modo de auditoria**, **exceções temporárias** e **recuperação por administrador sempre disponível**.
- **Ícone na área de notificação**, iniciar com o Windows (opcional), tema claro/escuro do Windows e cronômetro da sessão.
- **Privado por padrão:** nada sai do seu computador.

## Início rápido

Requer Windows 10 ou 11 (x64) e o Microsoft Edge WebView2 Runtime. **Verificado até agora apenas no Windows 10 Pro 22H2.** Veja a [compatibilidade](../compatibility.md) (em inglês).

1. Baixe o `Houseguest-<versão>-x64.msi` e o `SHA256SUMS.txt` na [última versão](https://github.com/UnkoynX777/houseguest/releases/latest).
2. Confira com `sha256sum -c SHA256SUMS.txt` (ou `Get-FileHash`). As versões também têm atestado de proveniência: `gh attestation verify <arquivo> --repo UnkoynX777/houseguest`.
3. Execute o instalador como administrador. Os binários **não são assinados**; o SmartScreen pode avisar.
4. Abra o Houseguest, defina um PIN de proprietário, crie uma conta padrão para o convidado e vá em **Sessões → Prévia → Iniciar proteção**.

Para recuperar o acesso, em um prompt elevado: `houseguest-cli recover` (com `--reset-owner` também remove o PIN). Com uma sessão ativa, o desinstalador recusa a remoção, a menos que um administrador use `FORCE_UNINSTALL=1`.

## Como funciona e onde para

O Houseguest observa a criação de processos pelo Event Tracing for Windows. Quando um programa inicia na conta do convidado, o serviço avalia as regras e encerra o processo se não for permitido. Se o Event Tracing estiver indisponível, passa a verificar os programas em execução a cada segundo e avisa você.

A aplicação é **reativa, não preventiva**: um programa bloqueado pode rodar por um instante antes de ser encerrado. O Houseguest não usa drivers de kernel, AppLocker, App Control nem o Windows Defender, então funciona da mesma forma em todas as edições.

| Quem | O que o Houseguest garante |
| --- | --- |
| Usuário padrão, janela fechada | As regras continuam valendo. O serviço, os dados e as configurações não podem ser parados, lidos ou editados sem direitos de administrador. |
| Convidado durante a sessão | A sessão termina no horário. Programas permitidos não alteram a política. |
| Administrador local | Adulteração é detectável. Remoção e recuperação sempre são possíveis, por projeto. |
| Acesso físico | Nada. Use BitLocker e senha de firmware. |

Programas permitidos podem iniciar outros programas; revise lançadores e navegadores. Aplicativos da Microsoft Store são identificados pelo caminho do executável ou pelo fabricante.

## Documentação e contribuição

Documentação (em inglês): [FAQ](../faq.md), [compatibilidade](../compatibility.md), [solução de problemas](../troubleshooting.md), [arquitetura](../architecture.md), [modelo de ameaças](../threat-model.md), [privacidade](../privacy.md). Contribuições são bem-vindas e revisadas por uma pessoa; leia o [CONTRIBUTING](../../CONTRIBUTING.md). Vulnerabilidades devem ser reportadas em particular, conforme o [SECURITY](../../SECURITY.md).

## Licença

Apache-2.0. Partes deste projeto foram escritas com ajuda de ferramentas de IA e revisadas pelo mantenedor.
