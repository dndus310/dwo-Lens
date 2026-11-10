# dwo-Forge QA 보고서

> dwo-Forge 2.0은 dwo-Lens의 새 이름입니다. 1~7절은 dwo-Lens 1.x 시절의 기록이라 그때의 이름(`__dwo-lens`, `StateMonitor` 등)을 그대로 둡니다. 2.0 검증은 [8절](#8-v20-dwo-forge-검증-2026-11-11)에 있습니다.

- 일자: 2026-10-28
- 엔진: UE 5.7.4 (Launcher)
- 시스템 커밋: 829a161 이후
- 재현: `tests\selftest.bat "<UE 프로젝트 폴더>"`, `python tests\stress.py`, `powershell -File tests\package_check.ps1 -Source <폴더> -Name <이름> -Config Shipping`

## 요약

| 구분 | 결과 |
|---|---|
| 단위 테스트 | 28/28 통과 (Python 도구 4종, 실제 추출 데이터 픽스처 포함, 약 2초) |
| 자체 검증 (selftest) | 26/26 통과 (정적 검사, GUI 빌드, 격리 복사본에서 전체 명령 왕복, 안전장치) |
| 스트레스 | 3/3 통과 (대형 분석 14.8초, 로그 100만 줄 7.6초, 20만 노드 순환 1.5초) |
| Shipping 패키징 | BP 전용 + StateMonitor, C++ + 자체 StateMonitor 모두 통과. 패키지에 dwo-Lens 흔적 0 |
| 발견·수정한 결함 | 9건 (아래 표) |

## 1. 단위 테스트 (`tests/test_tools.py`)

| 대상 | 검증 내용 |
|---|---|
| 로그 파서 | 심각도·카테고리 판별, ensure·assert 인식, Fatal 콜스택, 지문 그룹, 이전 실행 대비 새 그룹, 판정, 크래시 XML |
| 분석기 | SKEL 클래스 정규화, 외부 액터 패키지 접기, 순환 탐지, 도달 수 계산, 1,000단계 이상 사슬, 실제 temp_proj 추출 데이터로 그래프·SQLite·산출물, **같은 입력 두 번 실행 시 결과 바이트 동일** |
| 모듈 스캔 | Build.cs 의존성, 주석 처리된 의존성 제외, 모듈 순환 → fail |
| 품질 점검 | 설정 병합, 이름 규칙 6종, 미사용 에셋(맵·ini 시작점)·리다이렉터, BP 복잡도(루프 포함), StateMonitor 감지·미감지 |
| 검증 라인 | 판정 우선순위, BOM 붙은 index.json, 테스트 실패, 일치 테스트 없음, 리포트 없는 비정상 종료 |
| 카탈로그·설정 | 도구 ID 중복, 값 범위, 설정 섹션 |

## 2. 자체 검증 (`tests/selftest.ps1`)

대상 프로젝트를 격리 폴더(`.qa/selftest`)에 복사해 실행하므로 원본은 수정되지 않습니다.

| 그룹 | 항목 |
|---|---|
| unit | Python 단위 테스트 28개 |
| static | PowerShell 4개 구문 검사, batch 4개 CRLF 검사, JSON 2개 |
| gui | GUI 빌드, `--export-report` 종합 리포트 생성 |
| cycle | attach → git에 안 보임 → analyze → verify → logs → sm-install → analyze가 StateMonitor 인식 → StateMonitor 테스트 6개 통과 → sm-remove → update 후 clone 깨끗 → detach 후 흔적 0, git 상태 동일 |
| safety | 프로젝트 코드가 StateMonitor를 참조하면 sm-remove와 detach가 거부 |

## 3. 스트레스 (`tests/stress.py`, scale 1.0)

| 시나리오 | 입력 | 시간 | 메모리(Python 추적 기준) |
|---|---|---|---|
| 분석기 | 네이티브 클래스 3,000, BP 1,500 (그래프 노드 240,000), 에셋 30,000 (긴 사슬, 순환 포함) → 심볼 34,506 · 관계 429,488 | **14.8초** (계측 없이) | 863MB |
| 로그 파서 | 100만 줄, 72MB | 7.6초 (초당 약 128,000줄) | 215MB |
| 깊은 그래프 | 200,001노드 단일 순환, 20,000노드 실행 체인 | 1.5초 | 71MB |

분석기 리포트는 실행 흐름을 큰 순서로 60개만 넣어 2MB로 유지되고, 전체는 `flows/*.mmd`에 남습니다.

## 4. Shipping 패키징 (`tests/package_check.ps1`)

각 시나리오는 격리 복사본에 dwo-Lens를 부착한 상태에서 `BuildCookRun -clientconfig=Shipping`으로 패키징하고, 패키지를 검사한 뒤 20초 실행(`-nullrhi`)하고, 마지막에 제거합니다.

| 시나리오 | 패키징 | 패키지 안 dwo-Lens | StateMonitor | 실행 20초 | 제거 후 흔적 |
|---|---|---|---|---|---|
| A. BP 전용(temp_proj) + StateMonitor(기본: 에디터 전용) | 성공, 635MB | 파일 0 · 바이너리 문자열 0 · pak 항목 0 | 설명 파일만 포함, 모듈은 로드 안 됨 | 정상 | 0 |
| A0. BP 전용, StateMonitor 없음 (기준) | 성공, 635MB | 0 · 0 · 0 | - | 정상 | 0 |
| B. C++(SMHost) + 자체 StateMonitor | 성공 (95초), 551MB | 0 · 0 · 0 | 실행 파일에 포함 (프로젝트 소유이므로 정상) | 정상 | 0 |
| 음성 대조: BP 전용 + 런타임 StateMonitor를 .uproject 등록 없이 | 성공 | 0 · 0 · 0 | 실행 파일에 없음 | **종료 코드 1** | - |

음성 대조 결과가 StateMonitor 기본 설치를 에디터 전용으로 바꾼 근거입니다. 패키지 게임에서 쓰려면 `sm-install --runtime` 후 `.uproject`(BP 전용) 또는 `Build.cs`(C++)에 등록해야 합니다.

## 5. 발견·수정한 결함

| # | 결함 | 발견 경로 | 수정 커밋 |
|---|---|---|---|
| 1 | BuildPlugin이 clone 안에 `FilterPlugin.ini`를 만들어 `update`(git pull) 실패 | StateMonitor 설치 테스트 | 160548d |
| 2 | StateMonitor 소스가 `StrProperty.h`·`TextProperty.h`를 직접 include하지 않아 단독 빌드 실패 (SMHost에서는 PCH로 가려짐) | 단독 빌드 | c911feb |
| 3 | 저장소 루트를 경로 문자열로 비교해 리디렉션·정션 경로에서 부착 거부 | 패키징 QA | 6c73b1e |
| 4 | 순환 탐지·실행 체인 계산이 재귀라 1,000단계 이상에서 RecursionError | 단위 테스트 | 97678c7 |
| 5 | 하드 참조 로드 수 계산이 O(N²) → 대형 프로젝트에서 사실상 멈춤 | 스트레스 | 8d4208c |
| 6 | 리포트 생성이 BP마다 전체 관계를 훑어 대형 프로젝트 84초 → 4배 개선 | 스트레스 | 829a161 |
| 7 | 검사 플러그인 설명 파일이 패키지(pak)에 포함 (기본 활성 때문) → 기본 비활성 + `-EnablePlugins` | Shipping 패키징 | c2cf985 |
| 8 | 런타임 StateMonitor가 BP 전용 패키지 게임을 시작 시 종료시킴 → 기본 에디터 전용, `--runtime` 분리 | Shipping 패키징 | 8fa3565 |
| 9 | GUI 툴팁 문자열 리터럴 줄바꿈으로 빌드 실패 | GUI 빌드 | 3c8d85f |

QA 스크립트 자체의 오류(엔진 바이너리를 직접 실행, PowerShell 5.1 매개변수 기본값, stderr 처리)도 수정했습니다.

## 6. 검증하지 않은 것

- Development 구성 패키징 (Shipping만 수행)
- 패키지 실행은 `-nullrhi` 20초 생존 확인까지. 게임플레이·렌더링 검증은 하지 않음 (Shipping은 로그가 없음)
- `sm-install --runtime` 후 `.uproject`·`Build.cs`에 등록한 상태의 패키징
- C++ 프로젝트에 dwo-Lens가 StateMonitor를 설치했을 때의 에디터 C++ 빌드 (SMHost는 자체 StateMonitor가 있어 설치를 거부함)
- 메모리 수치는 Python 할당만 추적한 값 (프로세스 전체 아님)

## 7. v1.6 확장 검증 (2026-11-04)

이름을 dwo-Lens로 바꾸고, GUI를 작업 공간 형식으로 다시 만들고, StateMonitor 실시간 연결과 새 라인 5개를 넣은 뒤의 검증입니다.

| 구분 | 결과 |
|---|---|
| 이름 변경 | 파일·식별자·스키마·환경 변수(`DWO_LENS_HOME`)·플러그인(`DwoLensInspector`) 일괄 변경. temp_proj를 이전 이름으로 제거하고 새 이름으로 다시 부착, 실행 기록 16개 이전 |
| 단위 테스트 | 41/41 통과 (기존 28 + 새 라인 13) |
| 자체 검증 (selftest) | 26/26 통과 (verify 안에서 StateMonitor 테스트 12개 실행, 0 실패) |
| StateMonitor 자동화 테스트 | 12/12 통과 (기존 7 + `Model.ObjectCapture`, `Model.ObjectFilter`, `Live.HelloWorldRemoveLines`, `Live.SnapLine`, `Live.ParseCommands`, `Live.WatchedObjectsAndManualSources`) |
| StateMonitor 빌드 | BuildPlugin 경고 0, `-StrictIncludes`(PCH·unity 없음) 통과 |
| 실시간 프로토콜 | `tests/live_probe.py`: hello와 GameMode·GameState·PC0·PC0.PlayerState·PC0.Pawn 스냅샷 수신, 모든 줄이 유효한 JSON, pause·resync·알 수 없는 명령 처리 |
| 게임 → GUI 종단 | 헤드리스 게임(`-game -nullrhi`)이 GUI(127.0.0.1:47731)에 자동 연결, 객체 그래프에 5개 노드와 소유 관계 표시 |
| 뷰어 없을 때 | 게임 60fps 유지, 재시도 중 로그 없음, 20초 뒤 뷰어를 켜면 2초 안에 연결 |
| 새 라인 (temp_proj 복사본) | smoke 통과 (17초), perf 통과 (p95 7.6ms, 600프레임, 304초), sizes 통과 (31 MB, 2초), trace 통과 (154 MB, 24초), play 창 확인 |
| GUI | 경고 0 빌드, 9개 화면과 State Monitor 상태 전이·타임라인을 실제 데이터와 녹화로 화면 캡처 확인 |

### 검증하지 않은 것 (v1.6)

- 에디터 PIE(여러 클라이언트 포함)에서의 실시간 연결. 독립 실행 게임으로만 확인
- 레벨 이동 시 `world` 메시지, `watch` 클래스 관찰을 실제 맵에서
- 실패·크래시하는 맵의 스모크 실행 (단위 테스트로만), 여러 맵, 용량 기준 초과 프로젝트
- `perf`·`trace`의 `nullrhi` 경로
- Shipping·모놀리식 빌드에서 프로젝트 프로퍼티 판별


## 8. v1.7 설치·부착·제거 단순화 (2026-11-07)

GitHub zip으로 받은 폴더에서 부착이 실패한 문제(설치 폴더에 `.git`이 없음, 프로젝트가 홈 폴더의 빈 git 저장소 안에 있음)를 계기로 절차를 바꿨습니다.

| 변경 | 내용 |
|---|---|
| 설치 | `dwo-Lens.bat` 더블클릭 하나로 시작. 폴더 등록(`DWO_LENS_HOME`)과 창 빌드를 자동으로 처리. `setup.bat`은 선택 |
| 실행 방식 | 프로젝트에 시스템을 git clone하지 않고 설치 폴더에서 실행. zip으로 받아도 동작. 버전은 `VERSION` (+ git이면 커밋) |
| 부착 | 창의 부착 버튼, 프로젝트 끌어다 놓기, `dwo-Lens.bat attach <폴더>`. 프로젝트에 `__dwo-lens.bat` 실행 파일을 자동으로 넣음 |
| 형상관리 | 자기 저장소면 그 exclude, 상위 저장소 안이면 상위 exclude에 프로젝트 경로를 붙인 블록, git이 없으면 exclude 없음 |
| 제거 | `uninstall`: 플러그인·StateMonitor·실행 파일·`__dwo-lens`·exclude 블록 제거. 실행 기록과 dwo-Lens가 남긴 파일(성능 CSV)은 보존(프로젝트 밖으로 이동) 또는 삭제를 선택 |
| 로그 | 창에서 실행한 명령 출력을 `%LOCALAPPDATA%\dwo-Lens\logs`에 저장, 실패 알림에서 바로 열기 |

| 검증 | 결과 |
|---|---|
| 자체 검증 (selftest) | 33/33 통과 (기존 26 + git 없는 프로젝트 3, 상위 저장소 3, 실행 파일 갱신 1) |
| git 없는 프로젝트 + zip 설치 | 부착 → sizes → analyze → sm-install → uninstall --delete-data, 남은 파일 0 |
| 상위 저장소 안의 프로젝트 | 상위 exclude에 `/games/temp_proj/...` 블록, 상위 `git status` 변화 없음. uninstall --keep-data 후 exclude가 부착 전과 같고, 기록과 CSV가 `%LOCALAPPDATA%\dwo-Lens\uninstalled`로 이동 |
| 이전 버전으로 부착된 프로젝트 | update 시 `__dwo-lens\system` 복사본과 이름 없는 옛 exclude 블록을 새 방식으로 교체, 실행 기록 18개 유지, uninstall 후 남은 파일 0 |
| 제거 대화상자 | 지울 항목, 실행 기록 수·크기, 보존/삭제 선택을 화면 캡처로 확인 |

### 검증하지 않은 것 (v1.7)

- 명령줄 `uninstall`에서 선택을 묻는 대화형 입력 (자동 실행은 비대화형이라 `--keep-data`/`--delete-data`로만 확인)
- 실제 성능 캡처가 남긴 CSV의 제거 (기록 파일에 경로를 넣어 시뮬레이션)
- 창으로 끌어다 놓기와 실패 알림의 로그 파일 열기 (빌드와 코드 경로만 확인)

## 8. v2.0 dwo-Forge 검증 (2026-11-11)

dwo-Lens를 dwo-Forge로 바꾸고 세 도구(Lens · DwoStateMonitor · DwoCanvas)로 재구성한 뒤의 검증입니다. 엔진 실행은 모두 격리된 프로젝트 복사본과 별도 `LOCALAPPDATA`에서 했습니다.

| 구분 | 결과 |
|---|---|
| 단위 테스트 | 52/52 통과 (Python 도구 42 + `tests/test_forge_cli.py` 10: dwo-Lens 1.x 프로젝트 이전, 실행 중인 옛 실행 파일 넘겨주기, exclude 블록 교체, `%LOCALAPPDATA%` 이전, 새·옛 이름 모두 제거, DwoCanvas 제거 거부·강제, 프로젝트 소유 DwoCanvas 보호, uninstall이 DwoCanvas 유지, 검사기 실행) |
| CoreRedirects | 설치한 DwoStateMonitor에서 `unreal.load_class`로 `/Script/StateMonitor.StateMonitorLibrary`·`StateMonitorWidget`·`StateMonitorSubsystem`이 `/Script/DwoStateMonitor.*`로 연결됨 (플러그인 `Config/Engine.ini`가 Engine 설정에 합쳐져 UObject 초기화 전에 읽힘, 코드 등록 불필요) |
| 발견·수정 | BuildPlugin 패키지에 `Config/*.ini`(CoreRedirects)와 DwoCanvas `Schema/`가 빠짐 → 설치 때 소스에서 보충. 커맨드릿 리포트 경로가 엔진 폴더 기준 상대 경로 → 절대 경로로 변환. 엔진 자식 프로세스가 잡은 로그 → 공유 읽기 |
| DwoCanvas 종단 | 부착 → `canvas-install`(git에 보임) → `canvas-validate` → `canvas-import --doc Settings` (종료 코드 0, 위젯 162개, 리포트 경로 출력) → `canvas-remove` 거부(WBP_Settings가 DwoCanvasRuntime 사용) → `uninstall`이 DwoCanvas를 남김. 이때 플러그인 바이너리는 아래 빌드 문제로 에디터 대상만 빌드한 것을 캐시에 넣어 사용 |
| GUI | 빌드 경고 0, `--screenshot`으로 DwoCanvas·도구 화면 캡처(`.qa/forge/`) |

### 검증하지 않은 것 (v2.0)

- DwoCanvas의 `BuildPlugin`(게임 대상)은 `SDwoCanvasShape.h`의 `TStrongObjectPtr<UMaterialInstanceDynamic>`가 불완전 형식이라 실패합니다(DwoCanvas 소스 문제). 그래서 selftest의 `canvas-install` 단계는 실패합니다.
- OWTStateMonitor → DwoStateMonitor 클래스 리다이렉트는 `load_class`로 확인되지 않았습니다(패키지 리다이렉트만 확인). 1.x와 같은 항목입니다.
- 실제 Figma 내보내기 파일, 에디터 안에서의 가져오기·시각 비교, Shipping 패키징(`package_check.ps1`)은 이번에 다시 돌리지 않았습니다.
