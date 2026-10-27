# dwo-Assay

언리얼 프로젝트에 흔적 없이 부착되는 분석·로그·검증 팩토리입니다. 대상 프로젝트를 읽기만 하며, 언제 떼어 내도 프로젝트는 그대로 동작합니다.

현재 가동 라인 (AI 미사용): **아키텍처 리포트(`analyze`)**, **검증(`verify`)**, **로그(`logs`)**

## 설치 (PC당 1회)

```bat
setup.bat
```

`DWO_ASSAY_HOME` 환경 변수를 이 저장소로 등록합니다. 실행 후 새 터미널을 여세요.

## 프로젝트에 부착

1. `__dwo-assay.bat`을 대상 프로젝트 루트(`.uproject`가 있는 폴더)에 복사합니다.
2. 실행합니다.

```bat
__dwo-assay.bat attach     :: 부착 (엔진 버전별 첫 회만 플러그인 빌드로 몇 분 걸림)
__dwo-assay.bat analyze    :: ST-1~2 아키텍처 리포트 + 품질 점검
__dwo-assay.bat verify     :: ST-3 BP 전체 컴파일, 데이터 검증, 자동화 테스트
__dwo-assay.bat logs       :: ST-4 Saved/Logs, Saved/Crashes 분석 (이전 실행 대비 새 오류)
__dwo-assay.bat tools      :: 도구 카탈로그 (ready / planned)
__dwo-assay.bat gui        :: 제어판 창 열기
__dwo-assay.bat status     :: 부착 버전·엔진 확인
__dwo-assay.bat doctor     :: 환경 점검
__dwo-assay.bat update     :: 시스템 최신화
__dwo-assay.bat detach     :: 제거 (--keep-data 로 분석 데이터 보존)
```

## 제어판 (GUI)

명령어 대신 창에서 쓰려면 둘 중 하나를 실행합니다.

- dwo-Assay 폴더의 **`gui.bat`을 더블클릭** → 창이 열리면 `찾아보기...`로 프로젝트 폴더 선택. 부착되지 않은 프로젝트는 `부착` 버튼으로 부착합니다.
- 부착된 프로젝트에서 `__dwo-assay.bat gui`

| 영역 | 내용 |
|---|---|
| 왼쪽 | 프로젝트 정보(엔진, 시스템 커밋), 라인 버튼(분석·검증·로그), 관리(점검·상태·업데이트·도구·제거), 실행 중 취소 |
| 실행 로그 | 명령 출력이 실시간으로 표시. 끝나면 `결과 보기` 버튼 |
| 실행 이력 | 모든 실행의 라인·판정·소요 시간·공정별 판정. 두 번 클릭하면 결과로 이동 |
| 결과 | 공정 표, 발견 사항, BP 복잡도, 이름 규칙, 미사용 후보, StateMonitor, 테스트·로그 그룹. 필터 검색, `리포트 열기`로 UML·흐름 다이어그램 |

처음 실행할 때 Visual Studio C++ 도구로 한 번 빌드합니다(약 1분, `%LOCALAPPDATA%\dwo-Assay\gui-cache`). GUI는 `__dwo-assay.bat`을 그대로 실행하고 같은 결과 파일을 읽으므로 명령줄과 결과가 같습니다.

### 전제 조건

- 대상 프로젝트는 **자체 git 저장소**여야 합니다 (상위 폴더 저장소에 포함된 경우 거부).
- Epic Launcher 또는 소스 빌드로 프로젝트의 `EngineAssociation` 엔진이 설치되어 있어야 합니다.
- Python 3.10 이상, git 2.31 이상.
- GUI를 쓰려면 Visual Studio 2022(C++ 데스크톱 개발 워크로드). UE C++ 개발 환경이면 이미 있습니다.
- C++ 프로젝트는 에디터 바이너리가 빌드된 상태여야 Commandlet이 실행됩니다.

## 부착 시 프로젝트에 생기는 것

```
A/
├─ __dwo-assay.bat
├─ __dwo-assay/
│  ├─ system/          # 이 저장소의 clone (버전 = 커밋)
│  ├─ data/runs/<id>/  # 실행별 raw · out · run.json · 로그
│  ├─ config.json      # (선택) 프로젝트별 규칙 덮어쓰기
│  └─ state.json
└─ Plugins/__DwoAssayInspector/   # 미리 빌드된 에디터 전용 플러그인 (Source 없음)
```

모두 `.git/info/exclude`의 표식 블록으로 git에서 숨겨집니다. `.uproject`, `Build.cs`, 기존 파일은 건드리지 않습니다. 제거 시 엔진 작업 폴더(`Saved/`, `Intermediate/`, `DerivedDataCache/`)를 뺀 git 상태가 부착 전과 같은지 검사합니다.

## 라인과 도구

| 라인 | 공정 | 도구 |
|---|---|---|
| `analyze` | ST-1 수집 | `-run=DwoAssayInspector` (리플렉션, BP 그래프, 에셋 참조), 모듈·플러그인 스캔 |
| | ST-2 분석 | 통합 그래프, 순환 검사, 하드 참조 체인, 미사용 에셋·리다이렉터, BP 복잡도 |
| | ST-3 규칙 | 이름·폴더 규칙 (UE5 스타일 가이드 기본값) |
| | ST-4 연계 | StateMonitor 어댑터: 프로젝트에 있으면 제출 지점·Source ID 수집 |
| `verify` | ST-3 검증 | `CompileAllBlueprints`, `DataValidation`, `Automation RunTests` (StateMonitor가 있으면 `StateMonitor.*` 추가) |
| `logs` | ST-4 로그 | 구조화 로그 파서 (심각도·카테고리·지문 그룹, 이전 실행 대비 새 오류), 크래시 수집 |

전체 목록과 계획 중인 도구(Insights 내보내기, CSV 성능 리포트, 정적 분석 등)는 `tools/catalog.json` 또는 `__dwo-assay.bat tools`로 확인합니다.

## 설정

`config/default.json`이 기본값입니다. 프로젝트별로 바꾸려면 `__dwo-assay/config.json`에 같은 구조로 필요한 키만 적습니다.

```json
{
  "naming": { "forbidAssetsAtContentRoot": false },
  "verify": { "automationFilters": ["Project.", "MyGame."] }
}
```

## 구성

| 경로 | 역할 |
|---|---|
| `__dwo-assay.bat` | 부트스트랩: 사전 확인 → exclude → clone/pull → `assay.ps1` 호출 |
| `tools/assay.ps1` | 명령 실행, 엔진 호출, 실행 기록 (`run.json`) |
| `tools/assay_analyze.py` | raw JSON → 통합 그래프, SQLite, 다이어그램, 게이트, HTML 리포트 |
| `tools/assay_quality.py` | 이름 규칙, 미사용 에셋, BP 복잡도, StateMonitor 감지 |
| `tools/assay_logs.py` | UE 로그·크래시 파서 |
| `tools/assay_verify.py` | 검증·로그 라인 판정과 리포트 |
| `tools/catalog.json` | 도구 레지스트리 |
| `gui/`, `gui.bat`, `tools/gui.ps1` | ImGui 제어판 (Dear ImGui 1.91.9b, nlohmann/json 3.11.3, MIT) |
| `config/default.json` | 규칙과 기준값 |
| `plugin/DwoAssayInspector` | 추출 Commandlet (읽기 전용, 패키지를 저장하지 않음) |

플러그인은 엔진 버전과 소스 해시별로 `%LOCALAPPDATA%\dwo-Assay\plugin-cache`에 한 번만 빌드되고 여러 프로젝트가 재사용합니다.

## 산출물 (`__dwo-assay/data/runs/<id>/`)

| 파일 | 내용 |
|---|---|
| `run.json` | 실행 ID, 라인, 공정별 판정·소요 시간, 시스템 커밋 |
| `out/report.html` | 라인별 리포트 |
| `out/graph.json`, `out/index.db` | 심볼·관계 (status, evidence 포함) |
| `out/class_uml.puml` / `.mmd`, `out/flows/*.mmd` | 클래스 다이어그램, BP 실행 흐름 |
| `out/findings.json`, `out/quality.json`, `out/statemonitor.json` | 분석 판정과 품질 점검 |
| `out/verify.json`, `out/logs.json` | 검증·로그 라인 결과 |
