# Archi

언리얼 프로젝트에 흔적 없이 부착되는 분석·로그·검증 팩토리입니다. 대상 프로젝트를 읽기만 하며, 언제 떼어 내도 프로젝트는 그대로 동작합니다.

현재 단계: **Phase 1 — 아키텍처 리포트 라인** (AI 미사용)

## 설치 (PC당 1회)

```bat
setup.bat
```

`ARCHI_HOME` 환경 변수를 이 저장소로 등록합니다. 실행 후 새 터미널을 여세요.

## 프로젝트에 부착

1. `__archi.bat`을 대상 프로젝트 루트(`.uproject`가 있는 폴더)에 복사합니다.
2. 실행합니다.

```bat
__archi.bat attach     :: 부착 (처음 한 번은 플러그인 빌드로 몇 분 걸림)
__archi.bat analyze    :: 아키텍처 리포트 생성
__archi.bat status     :: 부착 버전·엔진 확인
__archi.bat doctor     :: 환경 점검
__archi.bat update     :: 시스템 최신화
__archi.bat detach     :: 제거 (--keep-data 로 분석 데이터 보존)
```

### 전제 조건

- 대상 프로젝트는 **자체 git 저장소**여야 합니다 (상위 폴더 저장소에 포함된 경우 거부).
- Epic Launcher 또는 소스 빌드로 프로젝트의 `EngineAssociation` 엔진이 설치되어 있어야 합니다.
- Python 3.10 이상, git 2.31 이상.

## 부착 시 프로젝트에 생기는 것

```
A/
├─ __archi.bat
├─ __archi/
│  ├─ system/          # 이 저장소의 clone (버전 = 커밋)
│  ├─ data/runs/<id>/  # 실행별 raw · out · run.json · 로그
│  └─ state.json
└─ Plugins/__ArchiInspector/   # 미리 빌드된 에디터 전용 플러그인 (Source 없음)
```

모두 `.git/info/exclude`의 표식 블록으로 git에서 숨겨집니다. `.uproject`, `Build.cs`, 기존 파일은 건드리지 않습니다.

## 구성

| 경로 | 역할 |
|---|---|
| `__archi.bat` | 부트스트랩: 사전 확인 → exclude → clone/pull → `archi.ps1` 호출 |
| `tools/archi.ps1` | attach / update / status / doctor / analyze / detach |
| `tools/archi_analyze.py` | raw JSON → 통합 그래프, SQLite, 다이어그램, 게이트, HTML 리포트 |
| `plugin/ArchiInspector` | 리플렉션·BP 그래프·에셋 참조를 추출하는 Commandlet (`-run=ArchiInspector`) |

플러그인은 엔진 버전과 소스 해시별로 `%LOCALAPPDATA%\Archi\plugin-cache`에 한 번만 빌드되고 여러 프로젝트가 재사용합니다.

## 산출물 (`__archi/data/runs/<id>/out`)

| 파일 | 내용 |
|---|---|
| `report.html` | 판정, 발견 사항, 클래스 UML, 모듈·에셋 그래프, BP 실행 흐름 |
| `graph.json`, `index.db` | 심볼·관계 (status, evidence 포함) |
| `class_uml.puml` / `.mmd` | 클래스 다이어그램 |
| `flows/*.mmd` | BP 그래프별 실행 흐름 |
| `findings.json` | 게이트 판정 (pass / warn / fail) |
