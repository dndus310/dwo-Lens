# dwo-Lens

언리얼 프로젝트에 흔적 없이 부착되는 분석·로그·검증 팩토리입니다. 대상 프로젝트를 읽기만 하며, 언제 떼어 내도 프로젝트는 그대로 동작합니다.

현재 가동 라인 (AI 미사용): **아키텍처 리포트(`analyze`)**, **검증(`verify`)**, **로그(`logs`)**, **맵 스모크(`smoke`)**, **성능(`perf`)**, **콘텐츠 용량(`sizes`)**, **Insights 트레이스(`trace`)**, 그리고 실시간 **State Monitor**

## 설치 (PC당 1회)

```bat
setup.bat
```

`DWO_LENS_HOME` 환경 변수를 이 저장소로 등록합니다. 실행 후 새 터미널을 여세요.

## 프로젝트에 부착

1. `__dwo-lens.bat`을 대상 프로젝트 루트(`.uproject`가 있는 폴더)에 복사합니다.
2. 실행합니다.

```bat
__dwo-lens.bat attach     :: 부착 (엔진 버전별 첫 회만 플러그인 빌드로 몇 분 걸림)
__dwo-lens.bat analyze    :: ST-1~2 아키텍처 리포트 + 품질 점검
__dwo-lens.bat verify     :: ST-3 BP 전체 컴파일, 데이터 검증, 자동화 테스트
__dwo-lens.bat logs       :: ST-4 Saved/Logs, Saved/Crashes 분석 (이전 실행 대비 새 오류)
__dwo-lens.bat smoke      :: 맵마다 -game -nullrhi로 띄워 Fatal·크래시·ensure·오류 검사
__dwo-lens.bat perf [맵]  :: CSV 프로파일러로 N 프레임 캡처, 프레임 시간 p95·히치를 예산과 비교
__dwo-lens.bat sizes      :: Content 용량 (에셋·폴더·형식), 하드 참조로 함께 로드되는 무게 (엔진 불필요)
__dwo-lens.bat trace [맵] :: Unreal Insights 트레이스(.utrace) 기록
__dwo-lens.bat play [맵]  :: 게임 창 실행 후 바로 반환 (pid 출력, 실행 기록 없음)
__dwo-lens.bat tools      :: 도구 카탈로그 (ready / planned)
__dwo-lens.bat gui        :: 제어판 창 열기
__dwo-lens.bat sm-install :: StateMonitor 설치 (아래 '제공 도구')
__dwo-lens.bat status     :: 부착 버전·엔진 확인
__dwo-lens.bat doctor     :: 환경 점검
__dwo-lens.bat update     :: 시스템 최신화
__dwo-lens.bat detach     :: 제거 (--keep-data 로 분석 데이터 보존)
```

## 제어판 (GUI)

명령어 대신 창에서 쓰려면 둘 중 하나를 실행합니다.

- dwo-Lens 폴더의 **`gui.bat`을 더블클릭** → `파일 > 프로젝트 열기`(Ctrl+O)로 프로젝트 폴더 선택. 부착되지 않은 프로젝트는 대시보드의 `부착` 버튼으로 부착합니다.
- 부착된 프로젝트에서 `__dwo-lens.bat gui`

화면은 IDE·프로파일러 형식입니다. 왼쪽 **액티비티 바**로 화면을 바꾸고, 각 화면은 **탐색(왼쪽) · 본문(가운데) · 속성(오른쪽) · 하단 패널**로 나뉩니다. 패널은 끌어서 다시 배치할 수 있고 `보기 > 레이아웃 초기화`로 되돌립니다.

| 화면 (단축키) | 내용 |
|---|---|
| 대시보드 (Ctrl+1) | 프로젝트 상태 배너, KPI 카드(새 문제, 발견, BP 복잡도, 테스트 통과율, 로그 오류, 맵 스모크, 프레임 p95, 콘텐츠 용량, State Monitor)와 실행별 추세선, 라인별 최근 10회 판정, 주요 문제, 시작하기 체크리스트 |
| 아키텍처 (Ctrl+2) | 클래스 계층·BP 실행 흐름·BP 호출·에셋 참조·모듈 의존성을 **노드 그래프**로 표시(이동·확대·미니맵·F 전체 보기), 품질 점검 표 |
| 문제 (Ctrl+3) | 모든 라인의 최신 실행에서 모은 문제. 심각도·범주 필터, 묶기, **NEW**(이전 실행 대비), 음소거(기준선) |
| 검증 (Ctrl+4) | 단계 카드, 자동화 테스트 트리, 최근 10회 결과 띠, **새 실패·불안정(flaky) 테스트**, 맵 스모크 결과 |
| 로그 (Ctrl+5) | 지문으로 묶은 로그 그룹, 횟수 추세, 처음·마지막 발생, 콜스택, 크래시 |
| 성능·용량 (Ctrl+6) | 프레임 시간 차트(예산선·스레드별), p95 추세, 히치 / 폴더별 용량 트리맵, 큰 에셋, 함께 로드되는 무게 / Insights 트레이스 열기 |
| State Monitor (Ctrl+7) | 실행 중인 게임의 상태를 실시간으로 표시 (아래 '제공 도구: StateMonitor') |
| 실행 기록 (Ctrl+8) | 실행별 공정 띠, 두 실행 **비교**(지표 변화, 새로 생긴·해결된 문제, 결과가 바뀐 테스트), 산출물 |
| 도구 (Ctrl+9) | 제공 도구 설치·제거, 라인 카드, 도구 카탈로그, 관리(점검·업데이트·제거) |

- **명령 팔레트 (Ctrl+K)**: 모든 화면 이동, 라인 실행, 설치, 리포트 저장을 검색해서 실행합니다.
- **상태 표시줄**: 실행 중인 명령과 진행 상황(취소), 오류·경고 수, State Monitor 연결 상태.
- **대기열**: 실행 중에 다른 라인을 누르면 대기열에 들어가 차례로 실행됩니다. 끝나면 알림이 뜨고 결과로 이동할 수 있습니다.

### HTML 리포트 저장

| 버튼 / 명령 | 결과 |
|---|---|
| 실행 기록 > 속성 `HTML 저장` | 선택한 실행의 `report.html`을 원하는 위치에 저장 |
| `파일 > 종합 리포트 저장...` | 라인마다 최신 실행의 리포트를 표지와 함께 HTML 파일 하나로 저장 (미완료 실행은 제외) |
| `dwo-lens-gui.exe --project <폴더> --export-report <파일.html>` | 창 없이 종합 리포트 생성 (자동화·CI용) |

처음 실행할 때 Visual Studio C++ 도구로 한 번 빌드합니다(약 1분, `%LOCALAPPDATA%\dwo-Lens\gui-cache`). GUI는 `__dwo-lens.bat`을 그대로 실행하고 같은 결과 파일을 읽으므로 명령줄과 결과가 같습니다.

## 제공 도구: StateMonitor

실행 중인 게임의 상태를 dwo-Lens 창으로 실시간 전송하는 런타임 플러그인입니다. **프로젝트 코드 없이** 동작합니다.

```bat
__dwo-lens.bat sm-install            :: 에디터·PIE 전용 (기본). 패키지 빌드에 영향 없음
__dwo-lens.bat sm-install --runtime  :: 패키지 게임에도 포함. .uproject(BP 전용) 또는 Build.cs(C++) 등록 필요
__dwo-lens.bat sm-remove             :: 제거. 프로젝트 코드가 참조 중이면 거부 (--force로 강제)
__dwo-lens.bat play [맵]              :: 게임 창을 띄움 (State Monitor가 자동 연결)
```

### 사용 순서

1. `sm-install`로 설치합니다(GUI: 도구 화면 또는 State Monitor 화면의 안내). `.uproject`는 수정하지 않으며 `Plugins/__StateMonitor`는 git에서 숨겨집니다.
2. dwo-Lens 창을 열고 **State Monitor** 화면(Ctrl+7)으로 갑니다. 창은 `127.0.0.1:47731`에서 기다립니다.
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
- 자세한 API와 프로토콜은 [plugin/StateMonitor/README.md](plugin/StateMonitor/README.md)에 있습니다.

설치하면 `analyze`가 제출 지점을 찾고 `verify`가 StateMonitor 테스트를 함께 실행합니다.

## 검증 (QA)

```bat
selftest.bat "C:\Projects\MyGame"                      :: 단위·정적·GUI·전체 명령 왕복 (격리 복사본에서)
python tests\stress.py                                    :: 대형 분석·100만 줄 로그·깊은 그래프
powershell -File tests\package_check.ps1 -Source <폴더> -Name pkg -Config Shipping [-InstallStateMonitor]
```

결과와 발견·수정한 결함은 [docs/QA-REPORT.md](docs/QA-REPORT.md)에 있습니다. 산출물은 `.qa/`(git 제외)에 생깁니다.

### 전제 조건

- 대상 프로젝트는 **자체 git 저장소**여야 합니다 (상위 폴더 저장소에 포함된 경우 거부).
- Epic Launcher 또는 소스 빌드로 프로젝트의 `EngineAssociation` 엔진이 설치되어 있어야 합니다.
- Python 3.10 이상, git 2.31 이상.
- GUI를 쓰려면 Visual Studio 2022(C++ 데스크톱 개발 워크로드). UE C++ 개발 환경이면 이미 있습니다.
- C++ 프로젝트는 에디터 바이너리가 빌드된 상태여야 Commandlet이 실행됩니다.

## 부착 시 프로젝트에 생기는 것

```
A/
├─ __dwo-lens.bat
├─ __dwo-lens/
│  ├─ system/          # 이 저장소의 clone (버전 = 커밋)
│  ├─ data/runs/<id>/  # 실행별 raw · out · run.json · 로그
│  ├─ config.json      # (선택) 프로젝트별 규칙 덮어쓰기
│  └─ state.json
└─ Plugins/__DwoLensInspector/   # 미리 빌드된 에디터 전용 플러그인 (Source 없음)
```

모두 `.git/info/exclude`의 표식 블록으로 git에서 숨겨집니다. `.uproject`, `Build.cs`, 기존 파일은 건드리지 않습니다. 제거 시 엔진 작업 폴더(`Saved/`, `Intermediate/`, `DerivedDataCache/`)를 뺀 git 상태가 부착 전과 같은지 검사합니다.

## 라인과 도구

| 라인 | 공정 | 도구 |
|---|---|---|
| `analyze` | ST-1 수집 | `-run=DwoLensInspector` (리플렉션, BP 그래프, 에셋 참조), 모듈·플러그인 스캔 |
| | ST-2 분석 | 통합 그래프, 순환 검사, 하드 참조 체인, 미사용 에셋·리다이렉터, BP 복잡도 |
| | ST-3 규칙 | 이름·폴더 규칙 (UE5 스타일 가이드 기본값) |
| | ST-4 연계 | StateMonitor 어댑터: 프로젝트에 있으면 제출 지점·Source ID 수집 |
| `verify` | ST-3 검증 | `CompileAllBlueprints`, `DataValidation`, `Automation RunTests` (StateMonitor가 있으면 `StateMonitor.*` 추가) |
| `logs` | ST-4 로그 | 구조화 로그 파서 (심각도·카테고리·지문 그룹, 이전 실행 대비 새 오류), 크래시 수집 |
| `smoke` | 맵 부팅 | `UnrealEditor-Cmd <맵> -game -nullrhi`를 `smoke.seconds`초 실행 후 종료, 로그 파서로 판정. fail: Fatal·크래시·ensure·비정상 종료·Error > `smoke.maxErrors` |
| `perf` | 성능 캡처 | `-ExecCmds="csvprofile frames=N"` + `-ExitAfterCsvProfiling` (CSV 프로파일러, `Saved/Profiling/CSV`), FrameTime·GameThread·RenderThread·RHIThread p50/p95/p99, 히치. fail: p95 > `perf.budgetMs` |
| `sizes` | 용량 | `.uasset/.umap/.uexp/.ubulk` 합계, 최신 `analyze`의 `graph.json`으로 하드 참조 폐포 무게. warn: 에셋 > `sizes.warnAssetMB` |
| `trace` | 트레이스 | `-trace=<trace.channels> -tracefile=<run>/out/trace.utrace`, `trace.seconds`초 후 창을 닫음. Unreal Insights로 열기 |
| `play` | (실행 아님) | `UnrealEditor <맵> -game -windowed 1280x720` 분리 실행. GUI에서 StateMonitor 뷰어로 상태를 받을 게임을 띄울 때 사용 |

맵 인수를 생략하면 `perf.map`, 그다음 `Config/DefaultEngine.ini`의 `GameDefaultMap`을 씁니다. 판정이 fail이면 종료 코드 2입니다.

전체 목록과 계획 중인 도구(Insights 내보내기, PerfReportTool 리포트, 정적 분석 등)는 `tools/catalog.json` 또는 `__dwo-lens.bat tools`로 확인합니다.

## 설정

`config/default.json`이 기본값입니다. 프로젝트별로 바꾸려면 `__dwo-lens/config.json`에 같은 구조로 필요한 키만 적습니다.

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
| `__dwo-lens.bat` | 부트스트랩: 사전 확인 → exclude → clone/pull → `lens.ps1` 호출 |
| `tools/lens.ps1` | 명령 실행, 엔진 호출, 실행 기록 (`run.json`) |
| `tools/lens_analyze.py` | raw JSON → 통합 그래프, SQLite, 다이어그램, 게이트, HTML 리포트 |
| `tools/lens_quality.py` | 이름 규칙, 미사용 에셋, BP 복잡도, StateMonitor 감지 |
| `tools/lens_logs.py` | UE 로그·크래시 파서 |
| `tools/lens_verify.py` | 검증·로그 라인 판정과 리포트 |
| `tools/lens_runtime.py` | 맵 부팅·성능 CSV·용량·트레이스 라인 판정과 리포트 |
| `tools/catalog.json` | 도구 레지스트리 |
| `gui/`, `gui.bat`, `tools/gui.ps1` | ImGui 제어판 (Dear ImGui 1.91.9b docking, nlohmann/json 3.11.3, MIT). State Monitor 뷰어 포함 |
| `config/default.json` | 규칙과 기준값 |
| `plugin/DwoLensInspector` | 추출 Commandlet (읽기 전용, 패키지를 저장하지 않음) |

플러그인은 엔진 버전과 소스 해시별로 `%LOCALAPPDATA%\dwo-Lens\plugin-cache`에 한 번만 빌드되고 여러 프로젝트가 재사용합니다.

## 산출물 (`__dwo-lens/data/runs/<id>/`)

| 파일 | 내용 |
|---|---|
| `run.json` | 실행 ID, 라인, 공정별 판정·소요 시간, 시스템 커밋 |
| `out/report.html` | 라인별 리포트 |
| `out/graph.json`, `out/index.db` | 심볼·관계 (status, evidence 포함) |
| `out/class_uml.puml` / `.mmd`, `out/flows/*.mmd` | 클래스 다이어그램, BP 실행 흐름 |
| `out/findings.json`, `out/quality.json`, `out/statemonitor.json` | 분석 판정과 품질 점검 |
| `out/verify.json`, `out/logs.json` | 검증·로그 라인 결과 |
| `out/smoke.json`, `out/perf.json` (+`perf.csv`), `out/sizes.json`, `out/trace.json` (+`trace.utrace`) | 맵 부팅·성능·용량·트레이스 라인 결과 |
