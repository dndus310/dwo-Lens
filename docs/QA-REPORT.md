# dwo-Assay QA 보고서

- 일자: 2026-10-28
- 엔진: UE 5.7.4 (Launcher)
- 시스템 커밋: 829a161 이후
- 재현: `selftest.bat "<UE 프로젝트 폴더>"`, `python tests\stress.py`, `powershell -File tests\package_check.ps1 -Source <폴더> -Name <이름> -Config Shipping`

## 요약

| 구분 | 결과 |
|---|---|
| 단위 테스트 | 28/28 통과 (Python 도구 4종, 실제 추출 데이터 픽스처 포함, 약 2초) |
| 자체 검증 (selftest) | 26/26 통과 (정적 검사, GUI 빌드, 격리 복사본에서 전체 명령 왕복, 안전장치) |
| 스트레스 | 3/3 통과 (대형 분석 14.8초, 로그 100만 줄 7.6초, 20만 노드 순환 1.5초) |
| Shipping 패키징 | BP 전용 + StateMonitor, C++ + 자체 StateMonitor 모두 통과. 패키지에 dwo-Assay 흔적 0 |
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

각 시나리오는 격리 복사본에 dwo-Assay를 부착한 상태에서 `BuildCookRun -clientconfig=Shipping`으로 패키징하고, 패키지를 검사한 뒤 20초 실행(`-nullrhi`)하고, 마지막에 제거합니다.

| 시나리오 | 패키징 | 패키지 안 dwo-Assay | StateMonitor | 실행 20초 | 제거 후 흔적 |
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
- C++ 프로젝트에 dwo-Assay가 StateMonitor를 설치했을 때의 에디터 C++ 빌드 (SMHost는 자체 StateMonitor가 있어 설치를 거부함)
- 메모리 수치는 Python 할당만 추적한 값 (프로세스 전체 아님)
