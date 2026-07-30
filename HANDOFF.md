# Handoff — saída dedicada para display externo no iOS

## Objetivo e comportamento

Esta alteração adiciona uma saída dedicada para monitor/TV usando o ciclo de
`UIWindowScene` oficial do UIKit. O renderer Metal existente muda de uma
`ARMSX2GameView` para outra; não há segundo renderer, captura de tela nem cópia
de frame.

O único controle novo é `Dedicated HDMI Output`, em **Graphics > Display**.
Ele persiste em:

```ini
[ARMSX2iOS/UI]
DedicatedExternalDisplay = false
```

O switch autoriza a função, mas não exige que um monitor esteja presente.

| Switch | VM/jogo | Display externo | Resultado |
|---|---|---|---|
| Off | qualquer estado | desconectado | render normal no iPhone |
| Off | qualquer estado | conectado | comportamento padrão de espelhamento do iOS |
| On | parado | desconectado/conectado | menu no iPhone; saída dedicada ainda não é solicitada |
| On | rodando | desconectado | render normal no iPhone |
| On | rodando | conecta (hot-plug) | renderer existente migra para o display externo |
| On | rodando | desconecta | renderer migra de volta para o iPhone |
| On → Off | rodando | conectado | renderer volta ao iPhone; janela externa é removida e o iOS pode espelhar |
| Off → On | rodando | conectado | cena/janela externa é ativada e recebe o renderer |

O display externo recebe apenas o frame do jogo. Os controles, menus e overlays
SwiftUI permanecem no iPhone; FullscreenUI/OSD/ImGui do core não são compostos
enquanto o alvo dedicado está ativo.

## Arquitetura, ownership e threads

- `g_gameRenderView` continua sendo a view estável que o SwiftUI incorpora.
- `s_externalGameRenderView` pertence à hierarquia de uma `UIWindow` externa
  preta.
- `s_activeGameRenderView` é um ponteiro não-owning, acessado no main thread,
  que escolhe a view entregue por `Host::AcquireRenderWindow()`.
- O único `GSDeviceMTL` troca a superfície com
  `MTGS::UpdateDisplayWindow()`. `GSDeviceMTL::UpdateWindow()` já destrói a
  superfície anterior, adquire a nova view e anexa a nova `CAMetalLayer`.
- UIKit, `UIWindowScene`, `UIWindow` e seleção de `UIScreenMode` ficam no main
  thread.
- O ring do MTGS tem o CPU thread como único produtor. Mudanças vindas do
  UIKit passam por `Host::RunOnCPUThread()` antes de enfileirar no GS thread.
- A `UIWindow` retirada mantém o retain de `alloc` até um comando posterior no
  GS thread confirmar que o update anterior já foi processado. Só então ela é
  ocultada, recebe `windowScene = nil` e é liberada no main thread. Isso evita
  liberar a `CAMetalLayer` enquanto o Metal ainda a usa.
- O worker persistente agora também acorda para tarefas do CPU thread enquanto
  está sem boot ativo; isso permite responder a toggle/hot-plug nas bordas do
  ciclo da VM sem um segundo produtor do ring.
- `Host::OnVMStarting()` marca a apresentação como solicitada.
  `Host::OnVMDestroyed()` desmarca e volta ao iPhone.

## iOS 26 versus iOS 27+

### iOS 26

O sistema oferece automaticamente uma cena com role
`UIWindowSceneSessionRoleExternalDisplayNonInteractive`.
`AppDelegate.mm` devolve `PCSX2ExternalDisplaySceneDelegate` somente para essa
role. O delegate é leve e nunca inicializa SDL, SwiftUI ou a VM.

Com o switch desligado, o delegate não anexa `UIWindow`, preservando o
espelhamento. Com o switch ligado e a VM solicitando vídeo, ele anexa a janela
externa.

### iOS 27 e posteriores

Quando o código é compilado com um SDK que define `__IPHONE_27_0`, a cena é
registrada no root view controller com:

```objc
UISceneAccessory.externalNonInteractiveSceneAccessoryWithConfiguration:
UIViewController.registerSceneAccessory:
UISceneAccessoryRegistration.enabled
```

O registro fica habilitado somente quando:

```text
DedicatedExternalDisplay && VMRequested
```

`UIViewController.unregisterSceneAccessory:` é usado ao desconectar a cena
principal. Todo acesso está duplamente protegido:

```objc
#if defined(__IPHONE_27_0) &&
    __IPHONE_OS_VERSION_MAX_ALLOWED >= __IPHONE_27_0
if (@available(iOS 27.0, *)) { ... }
#endif
```

Assim, um build feito com SDK 27 roda no iOS 26 pelo caminho antigo e no iOS
27+ pelo scene accessory. Um build feito com SDK 26 compila sem referenciar os
símbolos novos, mas não pode oferecer o fluxo obrigatório de scene accessory
do iOS 27; para validar o requisito completo, use Xcode com SDK iOS 27.

Documentação principal:

- <https://developer.apple.com/documentation/uikit/presenting-content-on-a-connected-display>
- <https://developer.apple.com/documentation/uikit/uisceneaccessory>
- <https://developer.apple.com/documentation/uikit/uisceneaccessoryregistration>

## Arquivos e funções alterados

### Ciclo iOS e bridge

- `platforms/ios/app/src/main/cpp/IOS/AppDelegate.mm`
  - `application:configurationForConnectingSceneSession:options:` roteia a
    role externa para o delegate leve.
- `platforms/ios/app/src/main/cpp/IOS/PCSX2SceneDelegate.h`
  - declara `PCSX2ExternalDisplaySceneDelegate`.
- `platforms/ios/app/src/main/cpp/IOS/SceneDelegate.mm`
  - contém o estado da cena/janela externa;
  - `ARMSX2ConfigureExternalScreenMode`;
  - `ARMSX2ActivateExternalDisplay`;
  - `ARMSX2DeactivateExternalDisplay`;
  - `ARMSX2ApplyExternalDisplayState`;
  - `ARMSX2RetargetRenderer` e cleanup seguro da janela;
  - `ARMSX2SetDedicatedExternalDisplayEnabled`;
  - `ARMSX2SetExternalDisplayVMRequested`;
  - `ARMSX2RegisterExternalDisplayAccessoryIfNeeded`;
  - implementação do delegate externo;
  - wake/drain da fila de CPU durante o idle do worker.
- `platforms/ios/app/src/main/cpp/IOS/IOSRuntime.h`
  - declara o contrato entre lifecycle, render view e fila do CPU thread.
- `platforms/ios/app/src/main/cpp/IOS/HostImpls.mm`
  - `AcquireRenderWindow()` escolhe a view ativa e consulta a tela correta;
  - `GetDisplayRefreshRate()` consulta `UIWindowScene.screen`;
  - callbacks da VM ativam/desativam a solicitação externa;
  - `RunOnCPUThread()` acorda o worker idle com segurança.
- `platforms/ios/app/src/main/cpp/ios_main.mm`
  - mantém `s_activeGameRenderView`;
  - `ARMSX2GameView::layoutSubviews` só redimensiona o alvo ativo;
  - a layer externa usa as dimensões em pixels de `currentMode`;
  - expõe `ARMSX2HasPendingCPUThreadTasks()`.
- `platforms/ios/app/src/main/cpp/ARMSX2Bridge.h` e
  `platforms/ios/app/src/main/cpp/ARMSX2Bridge.mm`
  - adicionam `setDedicatedExternalDisplayEnabled:`.

### Setting e interface

- `platforms/ios/app/src/main/swift/Models/SettingsStore.swift`
  - setting INI persistente, default `false`, callback nativo live e reset.
- `platforms/ios/app/src/main/swift/Views/Settings/GraphicsSettingsView.swift`
  - único toggle novo e texto explicativo.

### Renderer, aspecto e OSD

- `pcsx2/GS/GS.h` e `pcsx2/GS/GS.cpp`
  - flag atômica de apresentação externa;
  - `GSGetHostRefreshRate()` retorna vazio no modo dedicado para nunca alterar
    a velocidade da emulação em função do display.
- `pcsx2/GS/Renderers/Common/GSRenderer.h` e `GSRenderer.cpp`
  - helpers puros de resolução de aspecto e fit;
  - `Stretch` cai para `Auto` somente na saída dedicada;
  - `StretchY`, alinhamento deslocado e regra portrait/top não distorcem a
    saída dedicada;
  - fit centralizado reutiliza o cálculo do renderer e deixa barras pretas.
- `pcsx2/GS/Renderers/Metal/GSDeviceMTL.mm`
  - finaliza o frame ImGui, mas não o desenha no alvo externo.
- `tests/ctest/core/GS/external_display_aspect_tests.cpp` e CMake do diretório
  - regressão 4:3 em 1920×1080 (1440×1080, 240 px por lado), 16:9 e fallback
    de Stretch.

## Aspecto, barras, overlays e sincronização

- 4:3, 16:9, 10:7, 21:9, custom e Auto continuam usando a configuração atual
  do core.
- No alvo externo, o retângulo é centralizado e fitted; ele nunca é ampliado
  fora da tela nem deformado.
- `Stretch` e `StretchY` são ignorados somente durante a saída dedicada. As
  preferências gravadas do usuário não são alteradas.
- A janela e sua root view são pretas. O present pass Metal já limpa o drawable
  antes de desenhar, formando pillarbox/letterbox.
- SwiftUI não participa da janela externa.
- FullscreenUI, OSD e o draw data ImGui não são compostos externamente.
- O app registra resolução e `maximumFramesPerSecond`, mas não altera
  `NominalScalar`, FPS NTSC/PAL nem a velocidade da VM.
- Mesmo se `SyncToHostRefreshRate` estiver configurado, o modo externo faz
  `GSGetHostRefreshRate()` retornar `nullopt`; um monitor de 120 Hz apenas
  repete frames de um jogo de 50/60 FPS.

## Resolução e limite honesto de Hz

`UIScreenMode` expõe `size` e `pixelAspectRatio`, não a frequência individual
de cada modo. O código seleciona `screen.preferredMode`, lê
`screen.currentMode.size` e registra `screen.maximumFramesPerSecond`.

Consequências:

- Se o iOS/adaptador expuser corretamente um modo preferido de 60/120 Hz, ele
  será usado sem acelerar o jogo.
- Não há API pública para distinguir, dentro de `availableModes`, que
  1920×1080 é 60 Hz enquanto 3840×2160 é 30 Hz.
- Portanto, a preferência desejada **1080p60 sobre 4K30 só pode ser
  implementada se o SDK/hardware passar a expor frequência por modo**. Não
  inferir Hz pela resolução nem criar uma tabela de adaptadores.
- Se o log mostrar `size=3840x2160 max_fps=30` em um adaptador que oferece
  1080p60, isso é uma limitação conhecida da API atual. Registrar o caso antes
  de mudar a política.

Referências:

- <https://developer.apple.com/documentation/uikit/uiscreenmode>
- <https://developer.apple.com/documentation/uikit/uiscreen/currentmode>
- <https://developer.apple.com/documentation/uikit/uiscreen/preferredmode>
- <https://developer.apple.com/documentation/uikit/uiscreen/maximumframespersecond>

## Gerar, abrir e compilar no Mac

Pré-requisitos:

- clone recursivo/submódulos inicializados;
- Xcode completo selecionado, não apenas Command Line Tools;
- plataforma iOS e Metal tools instalados;
- CMake 3.16 ou posterior;
- Xcode/SDK iOS 27 para compilar e validar o caminho scene accessory.

Na raiz do repositório:

```sh
git submodule update --init --recursive
sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
xcodebuild -version
xcrun --sdk iphoneos --find metal
xcrun --sdk iphoneos --find metallib
```

Gerar o projeto assinado automaticamente:

```sh
cd platforms/ios
TEAM_ID=SEU_APPLE_TEAM_ID \
BUNDLE_ID=com.seu.bundle.armsx2 \
./scripts/generate-ios-xcode.sh
open build-ios-xcode/ARMSX2iOS.xcodeproj
```

No Xcode:

1. Selecione o scheme `ARMSX2iOS`.
2. Selecione um iPhone físico.
3. Em Signing & Capabilities, confirme Team e bundle ID.
4. Faça primeiro um build `Release` ou `Debug` para localizar erros de API.

Build de compilação sem assinatura:

```sh
cd platforms/ios
./scripts/generate-ios-xcode.sh
xcodebuild \
  -project build-ios-xcode/ARMSX2iOS.xcodeproj \
  -scheme ARMSX2iOS \
  -configuration Release \
  -sdk iphoneos \
  CODE_SIGNING_ALLOWED=NO \
  CODE_SIGNING_REQUIRED=NO \
  CODE_SIGN_IDENTITY="" \
  build
```

Gerar o IPA sem assinatura usando o script existente:

```sh
cd platforms/ios
./scripts/build-ios-ipa.sh
```

Saída esperada:

```text
platforms/ios/build-ios-xcode/ARMSX2-iOS-unsigned.ipa
```

`build-ios-xcode/` é gerado e não deve ser commitado.

## Assinatura e JIT

- O target de device habilita `ARMSX2_ENABLE_JIT_ENTITLEMENTS` por padrão e
  aponta para `platforms/ios/app/src/main/cpp/Entitlements.plist`.
- O arquivo solicita `get-task-allow`, `allow-jit` e
  `allow-unsigned-executable-memory`.
- Uma conta/perfil Apple comum pode não autorizar todas essas capabilities.
  Compilar/instalar o app não garante que o JIT funcionará.
- Para rodar jogos, use o mesmo método de assinatura/launch/JIT-enabler que já
  funciona para este port (Xcode debug, sideload/debug server compatível etc.).
- Para isolar erros da feature, primeiro prove que o app compila. Depois trate
  provisioning/JIT separadamente; não remova os entitlements silenciosamente.

## Checklist no Mac

### Compilação

- [ ] `generate-ios-xcode.sh` termina sem erro.
- [ ] `ARMSX2iOS` compila com SDK iOS 27.
- [ ] Não há erro de availability para `UISceneAccessory`,
      `UISceneAccessoryRegistration`, `registerSceneAccessory:` ou
      `unregisterSceneAccessory:`.
- [ ] Swift encontra
      `ARMSX2Bridge.setDedicatedExternalDisplayEnabled(_:)`.
- [ ] `PCSX2ExternalDisplaySceneDelegate` aparece no binário/target.
- [ ] O teste `external_display_aspect_tests.cpp` compila no harness desktop
      quando `ENABLE_TESTS=ON` estiver disponível.
- [ ] Nenhum `.xcodeproj`, DerivedData ou `build-ios-xcode/` entra no commit.

### Testes em iPhone físico

- [ ] Toggle default Off após instalação/config limpa.
- [ ] Toggle persiste após fechar e abrir o app.
- [ ] Off + HDMI conectado mantém espelhamento.
- [ ] On + nenhum HDMI mantém o jogo no iPhone.
- [ ] On antes do boot + HDMI conectado manda o jogo para a TV ao iniciar.
- [ ] On durante o jogo + hot-plug migra sem travar/encerrar a VM.
- [ ] Desconectar durante gameplay devolve o frame ao iPhone.
- [ ] Desligar o toggle durante gameplay devolve o frame e restaura mirroring.
- [ ] Religá-lo durante gameplay reativa a saída dedicada.
- [ ] Pause/retomar, background/foreground e encerramento da VM não deixam a TV
      presa nem produzem uso após liberação da layer.
- [ ] Controles virtuais, quick menu, toasts, OSD, FPS e ImGui não aparecem na TV.
- [ ] 4:3 em TV 16:9 produz pillarbox centralizado.
- [ ] 16:9 preenche TV 16:9.
- [ ] Stretch configurado no app não deforma a saída dedicada.
- [ ] Testar 1080p60, 4K30/60 e 120 Hz quando houver hardware disponível.
- [ ] Confirmar pelo log que 120 Hz não altera a velocidade/FPS do jogo.
- [ ] Repetir ao menos os estados principais em um device iOS 26 e outro iOS 27+.

## Logs esperados

Filtre o console do Xcode por `ExternalDisplay`:

```text
[ExternalDisplay] lifecycle=automatic_external_scene ios<=26
[ExternalDisplay] lifecycle=scene_accessory ios=27+
[ExternalDisplay] dedicated_output=0|1
[ExternalDisplay] vm_requested=0|1
[ExternalDisplay] scene connected
[ExternalDisplay] mode=automatic size=<W>x<H> max_fps=<N>
[ExternalDisplay] renderer target=external size=<W>x<H> max_fps=<N>
[ExternalDisplay] renderer target=iphone
[ExternalDisplay] scene disconnected
```

`Host::AcquireRenderWindow` também deve mostrar:

```text
External=0|1, Size=<W>x<H>, Scale=<N>, Refresh=<N>
```

Sequência esperada em hot-plug:

```text
scene connected
mode=automatic ...
renderer target=external ...
Host::AcquireRenderWindow ... External=1 ...
```

Na desconexão:

```text
scene disconnected
renderer target=iphone
Host::AcquireRenderWindow ... External=0 ...
```

## Troubleshooting e pontos de correção

### API do iOS 27 não compila

1. Confirme `xcodebuild -version` e o SDK com:

   ```sh
   xcrun --sdk iphoneos --show-sdk-version
   ```

2. Abra os headers do SDK e confira os nomes Objective-C reais de:
   `UISceneAccessory`, `UISceneAccessoryRegistration`,
   `externalNonInteractiveSceneAccessoryWithConfiguration:`,
   `registerSceneAccessory:`, `unregisterSceneAccessory:` e `enabled`.
3. Corrija somente o bloco protegido por
   `ARMSX2_HAS_IOS27_SCENE_ACCESSORY` em `IOS/SceneDelegate.mm`.
4. Mantenha tanto o guard de SDK quanto o `@available`; não exponha tipos do
   SDK 27 fora do `#if`.
5. A API ainda aparece como beta na documentação consultada; nomes podem mudar
   entre o SDK usado nesta implementação e o Xcode final.

### Bridge Swift falha

- Confirme que `ARMSX2-Bridging-Header.h` importa `ARMSX2Bridge.h`.
- A declaração Objective-C é:

  ```objc
  + (void)setDedicatedExternalDisplayEnabled:(BOOL)enabled;
  ```

- A chamada Swift esperada é:

  ```swift
  ARMSX2Bridge.setDedicatedExternalDisplayEnabled(value)
  ```

- Não duplique o setting em `UserDefaults`; a fonte persistente é o INI.

### Cena conecta, mas continua espelhando

- Confirme que switch e VM produziram `dedicated_output=1` e
  `vm_requested=1`.
- Confirme que o delegate externo foi instanciado e chamou
  `makeKeyAndVisible`.
- No iOS 27, inspecione `UISceneAccessoryRegistration.available` e `enabled`.
- Confirme que `AcquireRenderWindow` registrou `External=1`.

### TV fica preta

- Se `renderer target=external` existe mas não há
  `AcquireRenderWindow ... External=1`, audite a fila CPU → MTGS.
- Se o acquire ocorreu, coloque breakpoint em
  `GSDeviceMTL::UpdateWindow()`, `DestroySurface()` e
  `AttachSurfaceOnMainThread()`.
- Confirme que `currentMode.size`, bounds da view e `drawableSize` são maiores
  que zero.

### Crash ao desligar/desconectar

- Breakpoints: `ARMSX2DeactivateExternalDisplay`,
  `ARMSX2RetargetRendererOnCPUThread`,
  `GSDeviceMTL::DetachSurfaceOnMainThread` e
  `ARMSX2FinishRetiredExternalWindow`.
- A janela retirada só pode ser liberada depois do comando de update no GS
  thread. Não antecipe `[window release]`.

## Checks executados neste Windows

Executados com sucesso:

```text
git diff --check
```

- Resultado: exit code 0; nenhum whitespace error.
- Git somente avisou que o checkout Windows poderá converter LF para CRLF.

```text
parse XML: Info.plist.in e Entitlements.plist
```

- Resultado: ambos aceitos pelo parser XML do PowerShell.

```text
bash -n platforms/ios/scripts/generate-ios-xcode.sh
bash -n platforms/ios/scripts/build-ios-ipa.sh
```

- Resultado: ambos com sintaxe válida.

Também passaram verificações de contrato por busca:

- `SceneDelegate.mm` incluído em `IOS_RUNTIME_SOURCES`;
- declaração ObjC e chamada Swift do switch presentes;
- chave `ARMSX2iOS/UI/DedicatedExternalDisplay` presente;
- guards de SDK e runtime do iOS 27 presentes;
- role externa do iOS 26 e delegate externo presentes;
- teste de aspecto incluído no CMake.

Não executados neste Windows:

- geração do projeto Xcode;
- compilação Objective-C++, Swift ou Metal;
- link do app iOS;
- unit test C++ (não há compiler/Ninja/build configurado nesta máquina);
- assinatura, instalação, JIT ou execução;
- testes reais de HDMI, hot-plug, mirroring, resolução ou frequência.

Esses itens permanecem obrigatórios no Mac antes de considerar a feature
validada em hardware.
