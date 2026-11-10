# dwo-Forge

언리얼 프로젝트에 붙여 쓰는 도구 모음입니다. 프로젝트에 흔적 없이 부착되고, 세 가지 도구를 제공합니다.

| 도구 | 하는 일 | 프로젝트에 들어가는 것 |
|---|---|---|
| **Lens** | 분석·검증·로그 라인: 아키텍처 리포트(`analyze`), 검증(`verify`), 로그(`logs`), 맵 스모크(`smoke`), 성능(`perf`), 콘텐츠 용량(`sizes`), Insights 트레이스(`trace`) | `Plugins/__DwoLensInspector` (숨김, 미리 빌드, 기본 비활성) |
| **DwoStateMonitor** | 실행 중인 게임의 상태를 실시간으로 보는 런타임 플러그인 (예전 이름 StateMonitor) | `Plugins/__DwoStateMonitor` (숨김, 선택 설치) |
| **DwoCanvas** | 디자인 파일(`*.dwocanvas.json`)을 UMG 위젯 블루프린트로 가져오는 플러그인 | `Plugins/__DwoCanvas` (숨김, 선택 설치) |

Lens와 DwoStateMonitor는 프로젝트를 읽기만 하며, 언제 떼어 내도 프로젝트는 그대로 동작합니다. DwoCanvas는 위젯 에셋을 만드는 도구라서 프로젝트의 일부로 커밋합니다(아래 [DwoCanvas](#제공-도구-dwocanvas)).

> dwo-Lens에서 이름이 바뀌었습니다. dwo-Lens 1.x로 부착한 프로젝트는 dwo-Forge 명령을 처음 실행할 때(창에서 열 때 포함) 자동으로 옮겨집니다. 아래 [dwo-Lens에서 옮기기](#dwo-lens-1x에서-옮기기)를 보세요.

## 시작하기 (3단계)

1. dwo-Forge를 받습니다. GitHub의 **Download ZIP**을 풀어도 되고 `git clone`해도 됩니다. 어느 폴더든 상관없습니다.
2. 받은 폴더의 **`dwo-Forge.bat`을 더블클릭**합니다. 처음 한 번만 폴더를 등록하고 창을 빌드합니다(약 1분). 별도 설치 단계는 없습니다.
3. 창에서 프로젝트를 엽니다(`파일 > 프로젝트 열기`, 또는 프로젝트 폴더나 `.uproject`를 창에 **끌어다 놓기**). 대시보드의 **부착** 버튼을 누르면 끝입니다.

`dwo-Forge.bat`에 프로젝트 폴더나 `.uproject`를 끌어다 놓아도 그 프로젝트로 바로 열립니다. 이미 부착된 프로젝트는 프로젝트 안의 `__dwo-forge.bat`을 더블클릭해도 창이 열립니다. 예전 바로가기용 `dwo-Lens.bat`, `gui.bat`은 `dwo-Forge.bat`으로 넘겨 줍니다.

### 제거 (Uninstall)

창에서 `도구 > 제거`(또는 명령 팔레트 `Ctrl+K` → 제거)를 누르면 지울 항목과 실행 기록 크기를 보여 주고, **실행 기록·로그를 보존할지 삭제할지** 묻습니다.

| 지우는 것 | 내용 |
|---|---|
| 플러그인 | `Plugins/__DwoLensInspector`, dwo-Forge가 설치한 `Plugins/__DwoStateMonitor` (dwo-Lens 1.x의 `Plugins/__StateMonitor` 포함. 프로젝트 코드가 쓰고 있으면 제거를 멈추고 알려 줌), `Plugins/__DwoCanvas` (생성된 위젯이 쓰고 있으면 제거를 멈추고 알려 줌) |
| 실행 파일·설정 | `__dwo-forge.bat`, `__dwo-forge/` (부착 정보, 화면 설정, 문제 음소거), 남아 있으면 dwo-Lens 1.x의 `__dwo-lens.bat`, `__dwo-lens/` |
| git exclude | 부착 때 넣은 dwo-Forge 블록(과 dwo-Lens 1.x 블록)만 제거. 다른 내용은 그대로 |
| 실행 기록 | `__dwo-forge/data`와 dwo-Forge 실행이 프로젝트에 남긴 파일(성능 캡처 CSV). **보존**을 고르면 `%LOCALAPPDATA%\dwo-Forge\uninstalled\<프로젝트>-<시각>`으로 옮기고, **삭제**를 고르면 지움 |

**DwoCanvas는 남깁니다.** `canvas-import`로 만든 위젯 블루프린트가 `DwoCanvasRuntime` 모듈을 쓰므로, 지우면 그 에셋이 깨집니다. 필요 없어지면 `canvas-remove`로 따로 지웁니다.

제거 뒤 git 프로젝트는 `git status`가 부착 전과 같은지(DwoCanvas 제외), git이 없는 프로젝트는 dwo-Forge 파일이 하나도 남지 않았는지 검사합니다. `Saved/`, `Intermediate/`, `DerivedDataCache/`처럼 에디터가 원래 쓰는 폴더는 건드리지 않습니다.

### 명령줄

창 없이 쓰려면 받은 폴더의 `dwo-Forge.bat <명령> <프로젝트 폴더>`, 또는 부착된 프로젝트 안의 `__dwo-forge.bat <명령>`을 씁니다.

```bat
dwo-Forge.bat attach "C:\Projects\MyGame"   :: 부착 (엔진 버전별 첫 회만 플러그인 빌드로 몇 분 걸림)
__dwo-forge.bat                     :: 제어판 창 열기 (부착 전이면 부착)

:: Lens
__dwo-forge.bat analyze             :: 아키텍처 리포트 + 품질 점검
__dwo-forge.bat verify              :: BP 전체 컴파일, 데이터 검증, 자동화 테스트
__dwo-forge.bat logs                :: Saved/Logs, Saved/Crashes 분석 (이전 실행 대비 새 오류)
__dwo-forge.bat smoke               :: 맵마다 -game -nullrhi로 띄워 Fatal·크래시·ensure·오류 검사
__dwo-forge.bat perf [맵]           :: CSV 프로파일러로 프레임 캡처, p95·히치를 예산과 비교
__dwo-forge.bat sizes               :: Content 용량, 하드 참조로 함께 로드되는 무게 (엔진 불필요)
__dwo-forge.bat trace [맵]          :: Unreal Insights 트레이스(.utrace) 기록
__dwo-forge.bat play [맵]           :: 게임 창 실행 (DwoStateMonitor 연결용)

:: DwoStateMonitor
__dwo-forge.bat sm-install [--runtime]   :: 설치 (아래 '제공 도구: DwoStateMonitor')
__dwo-forge.bat sm-remove [--force]

:: DwoCanvas
__dwo-forge.bat canvas-install                         :: Plugins/__DwoCanvas에 설치 (git에서 숨김)
__dwo-forge.bat canvas-validate <파일>                  :: 디자인 파일 검사
__dwo-forge.bat canvas-import <파일> [--reset] [--doc 이름]  :: 위젯 블루프린트로 가져오기
__dwo-forge.bat canvas-remove [--force]                :: 제거 (생성된 위젯이 쓰고 있으면 거부)

:: 관리
__dwo-forge.bat status | doctor     :: 부착 정보와 세 도구 상태 · 환경 점검
__dwo-forge.bat update              :: dwo-Forge를 새 버전으로 바꾼 뒤 플러그인·실행 파일 갱신
__dwo-forge.bat tools               :: 도구 카탈로그
__dwo-forge.bat uninstall           :: 제거. 실행 기록 보존 여부를 물음 (--keep-data / --delete-data로 지정)
```

창에서 실행한 명령의 출력은 하단 **실행 로그** 패널에 표시되고, `%LOCALAPPDATA%\dwo-Forge\logs`에 파일로도 저장됩니다(최근 50개). 실패 알림의 **로그 파일 열기**로 바로 열 수 있습니다.

## 제어판 (GUI)

명령어 대신 창에서 쓰려면 둘 중 하나를 실행합니다.

- dwo-Forge 폴더의 **`dwo-Forge.bat`을 더블클릭** → 프로젝트를 열거나 창에 끌어다 놓기. 부착되지 않은 프로젝트는 대시보드의 `부착` 버튼으로 부착합니다.
- 부착된 프로젝트의 `__dwo-forge.bat`을 더블클릭

화면은 IDE·프로파일러 형식입니다. 왼쪽 **액티비티 바**는 세 도구별로 묶여 있고(**LENS · STATE · CANVAS · FORGE**), 각 화면은 **탐색(왼쪽) · 본문(가운데) · 속성(오른쪽) · 하단 패널**로 나뉩니다. 패널은 끌어서 다시 배치할 수 있고 `보기 > 레이아웃 초기화`로 되돌립니다.

| 묶음 | 화면 (단축키) | 내용 |
|---|---|---|
| Lens | 대시보드 (Ctrl+1) | 프로젝트 상태 배너, KPI 카드(새 문제, 발견, BP 복잡도, 테스트 통과율, 로그 오류, 맵 스모크, 프레임 p95, 콘텐츠 용량, DwoStateMonitor)와 실행별 추세선, 라인별 최근 10회 판정, 주요 문제, 시작하기 체크리스트 |
| Lens | 아키텍처 (Ctrl+2) | 클래스 계층·BP 실행 흐름·BP 호출·에셋 참조·모듈 의존성을 **노드 그래프**로 표시(이동·확대·미니맵·F 전체 보기), 품질 점검 표 |
| Lens | 문제 (Ctrl+3) | 모든 라인의 최신 실행에서 모은 문제. 심각도·범주 필터, 묶기, **NEW**(이전 실행 대비), 음소거(기준선) |
| Lens | 검증 (Ctrl+4) | 단계 카드, 자동화 테스트 트리, 최근 10회 결과 띠, **새 실패·불안정(flaky) 테스트**, 맵 스모크 결과 |
| Lens | 로그 (Ctrl+5) | 지문으로 묶은 로그 그룹, 횟수 추세, 처음·마지막 발생, 콜스택, 크래시 |
| Lens | 성능·용량 (Ctrl+6) | 프레임 시간 차트(예산선·스레드별), p95 추세, 히치 / 폴더별 용량 트리맵, 큰 에셋, 함께 로드되는 무게 / Insights 트레이스 열기 |
| DwoStateMonitor | DwoStateMonitor (Ctrl+7) | 실행 중인 게임의 상태를 실시간으로 표시 (아래 '제공 도구: DwoStateMonitor') |
| DwoCanvas | DwoCanvas (Ctrl+0) | 설치·제거 카드, 디자인 파일 고르기(열기 대화상자 또는 `*.dwocanvas.json` 끌어다 놓기), 검사·가져오기·초기화하고 가져오기, 최신 리포트 요약 (아래 '제공 도구: DwoCanvas') |
| dwo-Forge | 실행 기록 (Ctrl+8) | 실행별 공정 띠, 두 실행 **비교**(지표 변화, 새로 생긴·해결된 문제, 결과가 바뀐 테스트), 산출물 |
| dwo-Forge | 도구 (Ctrl+9) | 세 도구 카드(상태·설치·제거), Lens 라인 카드, 도구별로 묶은 카탈로그, 관리(점검·업데이트·제거) |

- **명령 팔레트 (Ctrl+K)**: 모든 화면 이동, 라인 실행, 설치, DwoCanvas 검사·가져오기, 리포트 저장을 검색해서 실행합니다.
- **상태 표시줄**: 실행 중인 명령과 진행 상황(취소), 오류·경고 수, DwoStateMonitor 연결 상태.
- **대기열**: 실행 중에 다른 명령을 누르면 대기열에 들어가 차례로 실행됩니다. 끝나면 알림이 뜨고 결과로 이동할 수 있습니다.

### HTML 리포트 저장

| 버튼 / 명령 | 결과 |
|---|---|
| 실행 기록 > 속성 `HTML 저장` | 선택한 실행의 `report.html`을 원하는 위치에 저장 |
| `파일 > 종합 리포트 저장...` | Lens 라인마다 최신 실행의 리포트를 표지와 함께 HTML 파일 하나로 저장 (미완료 실행은 제외) |
| `dwo-forge-gui.exe --project <폴더> --export-report <파일.html>` | 창 없이 종합 리포트 생성 (자동화·CI용) |

처음 실행할 때 Visual Studio C++ 도구로 한 번 빌드합니다(약 1분, `%LOCALAPPDATA%\dwo-Forge\gui-cache`). GUI는 `tools/forge.ps1`을 그대로 실행하고 같은 결과 파일을 읽으므로 명령줄과 결과가 같습니다.

## 제공 도구: DwoStateMonitor

실행 중인 게임의 상태를 dwo-Forge 창으로 실시간 전송하는 런타임 플러그인입니다. **프로젝트 코드 없이** 동작합니다.

```bat
__dwo-forge.bat sm-install            :: 에디터·PIE 전용 (기본). 패키지 빌드에 영향 없음
__dwo-forge.bat sm-install --runtime  :: 패키지 게임에도 포함. .uproject(BP 전용) 또는 Build.cs(C++)에 DwoStateMonitor 등록 필요
__dwo-forge.bat sm-remove             :: 제거. 프로젝트 코드가 참조 중이면 거부 (--force로 강제)
__dwo-forge.bat play [맵]              :: 게임 창을 띄움 (DwoStateMonitor가 자동 연결)
```

### 사용 순서

1. `sm-install`로 설치합니다(GUI: 도구 화면 또는 DwoStateMonitor 화면의 안내). `.uproject`는 수정하지 않으며 `Plugins/__DwoStateMonitor`는 git에서 숨겨집니다.
2. dwo-Forge 창을 열고 **DwoStateMonitor** 화면(Ctrl+7)으로 갑니다. 창은 `127.0.0.1:47731`에서 기다립니다.
3. 에디터에서 **Play(PIE)** 하거나, `게임 실행` 버튼(`play`)으로 게임을 띄웁니다. 게임이 시작되면 자동으로 연결됩니다(PIE 클라이언트가 여럿이면 각각 세션으로 표시).
4. GameMode, GameState, PlayerController, PlayerState, Pawn과 컴포넌트가 노드로 나타나고, 값이 바뀌면 깜빡입니다. 기본은 프로젝트(BP·C++)가 선언한 프로퍼티만 보내며, `엔진 프로퍼티 포함`을 켜면 전체를 보냅니다.

### 보는 방법

| 보기 | 내용 |
|---|---|
| 객체 그래프 | 상태 소스마다 노드 하나. 실선은 소유(컨트롤러→Pawn→컴포넌트), 점선은 프로퍼티가 가리키는 참조. 방금 바뀐 필드는 노란색 |
| 상태 전이 | 열거형·bool·이름·작은 정수 필드를 상태 머신으로 표시. 현재 상태는 초록색 테두리와 경과 막대, 전이선 두께와 라벨은 전이 횟수, 방금 일어난 전이는 깜빡임 |
| 속성 | 현재 값 트리(이전 값 툴팁), 변경 이력, 상태 필드 목록, JSON |
| 타임라인 (하단) | 소스별 변경 표시, 선택한 상태 필드의 값 구간. 재생 위치를 끌면 그 시점의 값으로 모든 보기가 바뀜. 이전·다음 변경으로 이동 |
| 이벤트 (하단) | 모든 변경을 시간 역순으로. 클릭하면 그 시점으로 이동 |

- **대상 추가**: 왼쪽 `관찰할 클래스`에 `BP_Enemy_C` 같은 클래스 이름을 넣으면 그 액터들을 함께 관찰합니다. Blueprint에서 `Watch State Object`(객체) 또는 `Submit State Snapshot`(구조체) 노드로 직접 추가할 수도 있습니다.
- **녹화**: 받은 메시지를 JSON Lines로 저장하고 나중에 다시 열어 같은 화면으로 볼 수 있습니다.
- **명령줄 스위치(게임)**: `-StateMonitorPort=N` 다른 포트, `-StateMonitorLive=0` 연결 끔, Shipping 빌드는 `-StateMonitorLive=1`일 때만 연결.
- 자세한 API와 프로토콜은 [plugin/DwoStateMonitor/README.md](plugin/DwoStateMonitor/README.md)에 있습니다.

설치하면 `analyze`가 제출 지점을 찾고 `verify`가 `StateMonitor.*` 테스트를 함께 실행합니다.

**StateMonitor에서 이름이 바뀌었습니다.** 플러그인·모듈 이름만 `DwoStateMonitor`이고 클래스·헤더 이름(`UStateMonitorSubsystem` 등)은 그대로입니다. 저장된 블루프린트의 `/Script/StateMonitor` 참조는 플러그인의 CoreRedirects가 새 모듈로 연결합니다. C++ 프로젝트는 `Build.cs`의 `"StateMonitor"`를 `"DwoStateMonitor"`로 바꿔야 하며, `status`와 `doctor`가 남은 곳을 경고합니다.

## 제공 도구: DwoCanvas

Figma·HTML 변환기·직접 작성 등 어디서 왔든 같은 형식(`dwocanvas/2`)의 디자인 파일 `*.dwocanvas.json`을 UMG **위젯 블루프린트 디자이너 트리**로 만듭니다. 다시 가져오면 디자인이 바뀐 곳만 갱신하고, UE에서 고친 위젯은 유지합니다. 예전 `designbridge/1` JSON도 읽습니다.

```bat
__dwo-forge.bat canvas-install                          :: Plugins/__DwoCanvas에 설치 (미리 빌드한 에디터 바이너리 포함, git에서 숨김)
__dwo-forge.bat canvas-validate Shop.dwocanvas.json      :: 스키마·규칙 검사 (엔진 불필요, 몇 초)
__dwo-forge.bat canvas-import Shop.dwocanvas.json        :: 가져오기. 리포트 경로를 출력
__dwo-forge.bat canvas-import Shop.dwocanvas.json --reset --doc Shop   :: UE 수정분까지 덮어쓰기, 문서 하나만
__dwo-forge.bat canvas-remove                           :: 제거 (Content의 .uasset이 DwoCanvasRuntime을 쓰면 거부, --force로 강제)
```

- **설치 방식은 DwoStateMonitor와 같습니다.** 원본은 dwo-Forge의 `plugin/DwoCanvas`에 있고, `canvas-install`이 `Plugins/__DwoCanvas`에 복사합니다(git에서 숨김). `canvas-remove`와 `uninstall`이 지웁니다.
- 생성된 위젯은 런타임 모듈 `DwoCanvasRuntime`(`UDwoCanvasShape`, `UDwoCanvasFrame`)을 씁니다. 그런 에셋이 남아 있으면 제거를 멈추고 목록을 보여 줍니다(`--force`로 강제). 그 위젯을 여는 다른 PC에도 DwoCanvas를 설치해야 합니다.
- 2.0.0이 `Plugins/DwoCanvas`에 설치한 것은 다음 명령 때 `Plugins/__DwoCanvas`로 옮겨집니다.
- Blueprint 전용 프로젝트도 미리 빌드한 에디터 바이너리가 들어가서 에디터가 컴파일 없이 열립니다.
- `canvas-import`는 `UnrealEditor-Cmd <uproject> -run=DwoCanvasImport -File=<json> [-ResetOverrides] [-Doc=<이름>]`를 실행합니다. 오류가 없으면 종료 코드 0이고, 리포트는 `<프로젝트>/Saved/DwoCanvas/Reports/*.txt`에 저장됩니다. 에디터를 닫고 실행하세요.
- `canvas-validate`는 `python -m dwocanvas validate <파일> [--json]`(`PYTHONPATH=canvas/python`)를 실행합니다.
- 창에서는 **DwoCanvas** 화면(Ctrl+0)에서 같은 일을 합니다. 디자인 파일을 창에 끌어다 놓으면 바로 그 화면으로 열리고, 명령 출력은 하단 실행 로그에, 최신 리포트의 상태·개수·쓴 에셋·오류와 경고는 본문에 요약됩니다.
- 사용법과 매핑 규칙: [plugin/DwoCanvas/README.md](plugin/DwoCanvas/README.md) · 설계(스키마 우선 구조): [canvas/docs/SchemaFirstArchitecture.md](canvas/docs/SchemaFirstArchitecture.md)

## dwo-Lens 1.x에서 옮기기

dwo-Forge 2.0은 dwo-Lens 1.x가 남긴 것을 스스로 옮깁니다. 프로젝트에 대해 어떤 명령이든(창에서 프로젝트를 여는 것 포함) 처음 실행할 때:

| dwo-Lens 1.x | dwo-Forge |
|---|---|
| `__dwo-lens/` (실행 기록, 설정, 부착 정보) | `__dwo-forge/` 로 이름 변경, 내용 유지 |
| `__dwo-lens.bat` | `__dwo-forge.bat` 설치. 옛 실행 파일이 실행 중이면 한 번만 새 실행 파일로 넘겨 주는 파일로 바뀐 뒤 스스로 지워짐 |
| git exclude의 `dwo-lens managed` 블록 | `dwo-forge managed` 블록으로 교체 |
| `%LOCALAPPDATA%\dwo-Lens` (캐시, 창 설정, 보존한 실행 기록) | `%LOCALAPPDATA%\dwo-Forge` |
| `Plugins/__StateMonitor` | `update` 또는 `sm-install` 때 같은 모드의 `Plugins/__DwoStateMonitor`로 교체 |
| 환경 변수 `DWO_LENS_HOME` | `DWO_FORGE_HOME` (옛 변수도 계속 읽음) |

`tools/lens.ps1`은 옛 실행 파일을 위해 `tools/forge.ps1`로 넘겨 주기만 합니다. `uninstall`은 새 이름과 옛 이름을 모두 지웁니다.

## 검증 (QA)

```bat
selftest.bat "C:\Projects\MyGame"                      :: 단위·정적·GUI·전체 명령 왕복 (격리 복사본에서, DwoCanvas 포함)
python -m unittest discover -s tests                   :: 엔진 없이 도는 테스트 (Python 도구, 이전·DwoCanvas 명령 규칙)
python tests\stress.py                                    :: 대형 분석·100만 줄 로그·깊은 그래프
powershell -File tests\package_check.ps1 -Source <폴더> -Name pkg -Config Shipping [-InstallStateMonitor]
```

결과와 발견·수정한 결함은 [docs/QA-REPORT.md](docs/QA-REPORT.md)에 있습니다. 산출물은 `.qa/`(git 제외)에 생깁니다.

### 전제 조건

- 형상관리는 선택입니다. git 저장소인 프로젝트는 dwo-Forge 파일을 `.git/info/exclude`로 숨기고, 상위 폴더 저장소 안에 있으면 그 저장소의 exclude에 프로젝트 경로를 붙여 숨깁니다. git이 없는 프로젝트는 숨길 대상이 없으므로 exclude를 쓰지 않습니다. (DwoCanvas는 예외로 숨기지 않습니다.)
- Epic Launcher 또는 소스 빌드로 프로젝트의 `EngineAssociation` 엔진이 설치되어 있어야 합니다.
- Python 3.10 이상. git은 git 프로젝트일 때만 필요합니다.
- 창(GUI)을 쓰려면 Visual Studio 2022(C++ 데스크톱 개발 워크로드). UE C++ 개발 환경이면 이미 있습니다.
- C++ 프로젝트는 에디터 바이너리가 빌드된 상태여야 Commandlet이 실행됩니다.

## 부착 시 프로젝트에 생기는 것

```
A/
├─ __dwo-forge.bat                # 실행 파일 (dwo-Forge 설치 폴더의 도구를 실행)
├─ __dwo-forge/
│  ├─ home.txt                    # dwo-Forge 설치 폴더 위치
│  ├─ data/runs/<id>/             # Lens 실행별 raw · out · run.json · 로그
│  ├─ data/canvas/                # canvas-import 엔진 로그, 마지막 가져오기 (last.json)
│  ├─ created-files.txt           # dwo-Forge 실행이 프로젝트에 남긴 파일 (제거 때 처리)
│  ├─ config.json                 # (선택) 프로젝트별 규칙 덮어쓰기
│  ├─ gui.json                    # 화면 설정 (문제 음소거)
│  └─ state.json                  # 부착 정보 (버전, 형상관리 방식, 부착 전 git 상태)
├─ Plugins/__DwoLensInspector/    # Lens: 미리 빌드된 에디터 전용 플러그인 (Source 없음, 기본 비활성)
├─ Plugins/__DwoStateMonitor/     # (sm-install 했을 때) 숨김
└─ Plugins/__DwoCanvas/           # (canvas-install 했을 때) 숨김
```

도구 자체는 프로젝트에 복사하지 않고 dwo-Forge 설치 폴더에서 실행합니다. `.uproject`, `Build.cs`, 기존 파일은 건드리지 않습니다. 이전 버전처럼 `__dwo-forge/system` 복사본이 있으면 `update`나 부착 때 지웁니다.

## Lens 라인과 도구

| 라인 | 공정 | 도구 |
|---|---|---|
| `analyze` | ST-1 수집 | `-run=DwoLensInspector` (리플렉션, BP 그래프, 에셋 참조), 모듈·플러그인 스캔 |
| | ST-2 분석 | 통합 그래프, 순환 검사, 하드 참조 체인, 미사용 에셋·리다이렉터, BP 복잡도 |
| | ST-3 규칙 | 이름·폴더 규칙 (UE5 스타일 가이드 기본값) |
| | ST-4 연계 | DwoStateMonitor 어댑터: 프로젝트에 있으면 제출 지점·Source ID 수집 |
| `verify` | ST-3 검증 | `CompileAllBlueprints`, `DataValidation`, `Automation RunTests` (DwoStateMonitor가 있으면 `StateMonitor.*` 추가) |
| `logs` | ST-4 로그 | 구조화 로그 파서 (심각도·카테고리·지문 그룹, 이전 실행 대비 새 오류), 크래시 수집 |
| `smoke` | 맵 부팅 | `UnrealEditor-Cmd <맵> -game -nullrhi`를 `smoke.seconds`초 실행 후 종료, 로그 파서로 판정. fail: Fatal·크래시·ensure·비정상 종료·Error > `smoke.maxErrors` |
| `perf` | 성능 캡처 | `-ExecCmds="csvprofile frames=N"` + `-ExitAfterCsvProfiling` (CSV 프로파일러, `Saved/Profiling/CSV`), FrameTime·GameThread·RenderThread·RHIThread p50/p95/p99, 히치. fail: p95 > `perf.budgetMs` |
| `sizes` | 용량 | `.uasset/.umap/.uexp/.ubulk` 합계, 최신 `analyze`의 `graph.json`으로 하드 참조 폐포 무게. warn: 에셋 > `sizes.warnAssetMB` |
| `trace` | 트레이스 | `-trace=<trace.channels> -tracefile=<run>/out/trace.utrace`, `trace.seconds`초 후 창을 닫음. Unreal Insights로 열기 |
| `play` | (실행 아님) | `UnrealEditor <맵> -game -windowed 1280x720` 분리 실행. GUI에서 DwoStateMonitor 뷰어로 상태를 받을 게임을 띄울 때 사용 |

맵 인수를 생략하면 `perf.map`, 그다음 `Config/DefaultEngine.ini`의 `GameDefaultMap`을 씁니다. 판정이 fail이면 종료 코드 2입니다.

전체 목록과 계획 중인 도구(Insights 내보내기, PerfReportTool 리포트, 정적 분석 등)는 `tools/catalog.json` 또는 `__dwo-forge.bat tools`로 확인합니다. 카탈로그 항목은 `tool` 값(lens, statemonitor, canvas, forge)으로 묶입니다.

## 설정

`config/default.json`이 Lens 기본값입니다. 프로젝트별로 바꾸려면 `__dwo-forge/config.json`에 같은 구조로 필요한 키만 적습니다.

```json
{
  "naming": { "forbidAssetsAtContentRoot": false },
  "verify": { "automationFilters": ["Project.", "MyGame."] },
  "perf": { "map": "/Game/Maps/L_Main", "budgetMs": 16.6 },
  "smoke": { "exclude": ["/Game/Developers/*", "/Game/Test/*"], "maxErrors": 3 }
}
```

## 구성

| 경로 | 역할 |
|---|---|
| `dwo-Forge.bat` | 시작점: 폴더 등록(`DWO_FORGE_HOME`), 창 열기, 끌어다 놓은 프로젝트 열기, 명령줄 실행 |
| `dwo-Lens.bat`, `gui.bat` | 예전 바로가기용. `dwo-Forge.bat`으로 넘김 |
| `__dwo-forge.bat` | 프로젝트 실행 파일: `home.txt`의 설치 폴더에서 `forge.ps1` 실행, 제거되면 스스로 삭제 |
| `tools/forge.ps1` | 명령 실행, 엔진 호출, 실행 기록 (`run.json`), dwo-Lens 1.x 이전 |
| `tools/lens.ps1`, `tools/legacy-project-launcher.bat` | dwo-Lens 1.x 실행 파일 호환 (forge.ps1로 넘김) |
| `tools/lens_analyze.py` | Lens: raw JSON → 통합 그래프, SQLite, 다이어그램, 게이트, HTML 리포트 |
| `tools/lens_quality.py` | Lens: 이름 규칙, 미사용 에셋, BP 복잡도, DwoStateMonitor 감지 |
| `tools/lens_logs.py` | Lens: UE 로그·크래시 파서 |
| `tools/lens_verify.py` | Lens: 검증·로그 라인 판정과 리포트 |
| `tools/lens_runtime.py` | Lens: 맵 부팅·성능 CSV·용량·트레이스 라인 판정과 리포트 |
| `tools/catalog.json` | 도구 레지스트리 (세 도구별로 묶음) |
| `gui/`, `tools/gui.ps1` | ImGui 제어판 (Dear ImGui 1.91.9b docking, nlohmann/json 3.11.3, MIT). DwoStateMonitor 뷰어, DwoCanvas 화면 포함 |
| `config/default.json` | Lens 규칙과 기준값 |
| `plugin/DwoLensInspector` | Lens 추출 Commandlet (읽기 전용, 패키지를 저장하지 않음) |
| `plugin/DwoStateMonitor` | DwoStateMonitor 런타임 플러그인 |
| `plugin/DwoCanvas`, `canvas/` | DwoCanvas 플러그인, 검사기(`canvas/python`), 어댑터, 설계 문서 |

플러그인은 엔진 버전과 소스 해시별로 `%LOCALAPPDATA%\dwo-Forge\plugin-cache`에 한 번만 빌드되고 여러 프로젝트가 재사용합니다.

## 산출물 (`__dwo-forge/data/runs/<id>/`)

| 파일 | 내용 |
|---|---|
| `run.json` | 실행 ID, 라인, 공정별 판정·소요 시간, 시스템 커밋 |
| `out/report.html` | 라인별 리포트 |
| `out/graph.json`, `out/index.db` | 심볼·관계 (status, evidence 포함) |
| `out/class_uml.puml` / `.mmd`, `out/flows/*.mmd` | 클래스 다이어그램, BP 실행 흐름 |
| `out/findings.json`, `out/quality.json`, `out/statemonitor.json` | 분석 판정과 품질 점검 |
| `out/verify.json`, `out/logs.json` | 검증·로그 라인 결과 |
| `out/smoke.json`, `out/perf.json` (+`perf.csv`), `out/sizes.json`, `out/trace.json` (+`trace.utrace`) | 맵 부팅·성능·용량·트레이스 라인 결과 |
