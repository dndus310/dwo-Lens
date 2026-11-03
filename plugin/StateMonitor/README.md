# State Monitor

## 큰 그림

USTRUCT 값을 제출하면 필드 트리, JSON, 변경 이력을 보여 주는 런타임 읽기 전용 모니터입니다. 다른 플러그인이나 프로젝트 코드에 의존하지 않으므로 `Plugins/StateMonitor` 폴더를 어느 UE 5.7 프로젝트의 `Plugins`에 복사해도 바로 쓸 수 있습니다.

```cpp
// 어디서든 한 줄로 제출하고, 한 줄로 화면에 띄운다.
UStateMonitorSubsystem::Get(this)->Submit(TEXT("Player"), PlayerState);
UStateMonitorSubsystem::Get(this)->ShowMonitor();
```

## 코드 없이 쓰기 (dwo-Lens Live)

1. 아래 **설치**대로 플러그인을 넣습니다. 코드는 쓰지 않습니다.
2. dwo-Lens 뷰어를 켭니다. 뷰어는 `127.0.0.1:47731`에서 기다립니다.
3. PIE나 Standalone으로 게임을 실행합니다. 게임 인스턴스마다 뷰어에 접속하고(PIE 클라이언트가 여럿이면 각각), 뷰어가 자동 관찰을 켜면 다음 소스가 실시간으로 보입니다.

| Source ID | 대상 |
|---|---|
| `GameMode`, `GameState` | 현재 월드의 GameMode(서버만), GameState |
| `PC0`, `PC0.PlayerState`, `PC0.Pawn` | 로컬 PlayerController와 그 PlayerState, Pawn (`PC1`… 순서) |
| `PC0.Pawn.<컴포넌트>` | Pawn 컴포넌트 중 표시할 프로퍼티가 하나 이상 있는 것 |
| `<액터 이름>`, `<액터>.<컴포넌트>` | 뷰어가 `watch`로 지정한 클래스의 액터(최대 개수까지)와 Watch State Object로 넣은 객체 |

- 기본으로 **프로젝트 코드가 선언한 프로퍼티만** 담습니다. Blueprint 클래스의 변수와 엔진 폴더 밖 C++ 모듈의 `UPROPERTY`가 해당하며, 엔진 클래스(`AActor`, `APawn` 등)의 프로퍼티는 뷰어에서 `engine`을 켤 때만 담습니다. 그래서 엔진 기본 GameMode처럼 프로젝트 프로퍼티가 없는 객체는 빈 값(`{}`)으로 보입니다.
- Deprecated, 에디터 전용, 델리게이트, Blueprint 실행 프레임은 항상 뺍니다. 엔진 쪽 Transient는 빼고 프로젝트 쪽 Transient는 남깁니다.
- 객체 참조는 객체 경로 문자열입니다. 뷰어는 이 값과 다른 소스의 `object`를 이어 관계를 그립니다.
- 객체가 사라지거나 뷰어가 끊기면 자동 소스를 지웁니다. 같은 ID의 수동 Submit 소스는 덮어쓰지 않습니다.
- 서브시스템 모델의 소스 한도는 128개이고, 자동 관찰은 수동 Submit을 위해 16개를 남깁니다. 노드·깊이·문자열 한도는 `FStateMonitorSettings`를 그대로 따릅니다.
- 뷰어가 없으면 2초마다 조용히 다시 접속을 시도합니다. 게임을 멈추지 않고, 로그는 접속·끊김 때 한 줄씩만 남깁니다.

### 명령줄

| 스위치 | 의미 |
|---|---|
| `-StateMonitorLive=0` | Live 연결을 끕니다. |
| `-StateMonitorLive=1` | Shipping 빌드에서 Live 연결을 켭니다(Shipping 외에는 기본으로 켜짐). |
| `-StateMonitorPort=N` | 접속 포트(기본 47731). |

### Watch State Object

Blueprint에서 **Watch State Object** 노드에 아무 객체(액터, 컴포넌트, UObject)를 연결하면 자동 관찰 목록에 들어갑니다. 뷰어가 자동 관찰을 켰거나 모니터 오버레이가 보이는 동안 주기적으로 스냅샷을 찍고, 객체가 사라지면 소스도 지웁니다. **Unwatch State Object**로 뺍니다. C++에서는 `Monitor->WatchObject(Object)`, 한 번만 찍으려면 `Monitor->SubmitObjectSnapshot(TEXT("Boss"), FText(), BossActor)`를 씁니다.

### Live 프로토콜 v1

UTF-8 JSON Lines(한 줄에 JSON 하나, `\n`으로 구분)입니다. 게임이 TCP 클라이언트, 뷰어가 서버입니다.

게임 → 뷰어

| 메시지 | 내용 |
|---|---|
| `{"t":"hello","v":1,"project","world","netMode","pie","pid","engine","build","time"}` | 접속 직후. `netMode`는 Standalone/ListenServer/Client/DedicatedServer, `pie`는 PIE 인스턴스 번호 또는 -1 |
| `{"t":"world","world":"..."}` | 맵이 바뀌었을 때 |
| `{"t":"snap","id","label","type","kind","object","owner","seq","time","auto","value","changes","truncated"}` | 소스 하나. `kind`는 struct/object, `value`는 현재 값 JSON 객체, `changes`는 `[{"path","type","old","new","change"}]`(`path`는 `Health`, `Nested/Position/X` 형식, 루트는 빈 문자열). 바뀐 변경이 한도를 넘으면 `omittedChanges`가 붙습니다 |
| `{"t":"remove","id":"..."}` | 소스가 사라졌을 때 |

뷰어 → 게임 (모르는 메시지와 필드는 무시)

| 메시지 | 동작 |
|---|---|
| `{"t":"auto","on":true,"interval":0.25,"engine":false}` | 자동 관찰 켜기/끄기, 주기(초), 엔진 프로퍼티 포함 여부. 접속마다 꺼진 상태로 시작합니다 |
| `{"t":"watch","class":"BP_Enemy_C","max":16}` / `{"t":"unwatch","class":"..."}` | 클래스(부모 클래스 이름도 일치)의 액터를 최대 `max`개 관찰 |
| `{"t":"pause","on":true}` | 송신 정지. 다시 풀면 현재 상태를 모두 보냅니다 |
| `{"t":"resync"}` | 모든 소스를 지금 다시 보냅니다 |

송신은 0.1초마다, `seq`가 바뀐 소스만 보냅니다. 그 사이의 여러 변경은 `value`에 합쳐지고 `changes`에는 마지막 제출의 변경만 담깁니다. 메시지 생성은 `StateMonitorLiveProtocol.h`의 순수 함수입니다.

## 설치

1. `Plugins/StateMonitor` 폴더를 대상 프로젝트의 `Plugins`에 복사합니다.
2. 플러그인을 활성화합니다(`.uproject`의 `Plugins`에 `StateMonitor` 추가 또는 에디터 플러그인 창).
3. C++에서 쓰면 소비 모듈의 `Build.cs`에 `"StateMonitor"`를 추가합니다.

## 사용 방법

### C++

```cpp
#include "StateMonitorSubsystem.h"

if (UStateMonitorSubsystem* Monitor = UStateMonitorSubsystem::Get(this))
{
    Monitor->Submit(TEXT("Player"), PlayerState);                          // Label을 비우면 Source ID를 표시
    Monitor->Submit(TEXT("Wave"), WaveState, FText::FromString(TEXT("Wave")));
    Monitor->ToggleMonitor();                                              // 뷰포트 오른쪽에 표시/숨김
}
```

- `Get(WorldContext)`는 게임 인스턴스의 서브시스템을 찾습니다. 게임 인스턴스가 없으면 nullptr입니다.
- 모델과 이력은 게임 인스턴스에 속하므로 레벨이 바뀌어도 유지됩니다.
- 타입을 런타임에 알 때는 `SubmitStructSnapshot(SourceId, Label, StructType, StructData)`를 씁니다.

### Blueprint

| 노드 | 역할 |
|---|---|
| Submit State Snapshot | Snapshot 핀에 아무 구조체나 연결해 제출 |
| Watch / Unwatch State Object | 객체를 자동 관찰 목록에 넣고 빼기 |
| Show / Hide / Toggle State Monitor | 뷰포트에 모니터 표시 |
| Remove State Source, Clear State History | 소스·이력 정리 |

UMG에 직접 배치하려면 팔레트의 **State Monitor**(`UStateMonitorWidget`)를 추가합니다. `bUseSharedModel`을 켜면 위 노드로 제출한 값이 그 위젯에 표시되고, 끄면 위젯 자체의 `Submit Snapshot`으로 받은 값만 표시합니다.

### 직접 소유(고급)

모니터를 특정 화면에 묶어야 하면 `FStateMonitorModel`과 `SStateMonitor`를 직접 만듭니다.

```cpp
MonitorModel = MakeShared<FStateMonitorModel>();
MonitorModel->SubmitSnapshot(TEXT("MySystem"), FText::FromString(TEXT("My system")), FMyState::StaticStruct(), &State);
SNew(SStateMonitor).Model(MonitorModel);
```

## 동작 규칙

- 모든 호출은 게임 스레드에서 합니다. 제출할 때 값이 문자열·트리로 복사되므로 원본 주소를 보관하지 않습니다.
- `USTRUCT`나 객체(`UObject`) 안의 `UPROPERTY`만 읽습니다. 객체 참조는 경로나 null로 표시하고, 참조 대상 내부를 따라가지 않습니다.
- 중첩 구조체, 고정 배열, `TArray`, `TMap`, `TSet`, 숫자, bool, enum, 문자열, 이름, 텍스트, 객체 참조를 표시합니다. 그 밖의 프로퍼티는 Unreal 텍스트 표현을 씁니다.
- 같은 Source ID로 다시 제출하면 Current가 교체됩니다. 값이 같으면 이력을 추가하지 않습니다.
- 배열은 인덱스로 비교하므로 중간 삽입은 뒤쪽 원소의 변경으로 보일 수 있습니다.
- 노드·깊이·문자열·소스·이력 수에 한도가 있습니다(`FStateMonitorSettings`). 한도에 걸리면 뷰어가 표시합니다.
- 설정과 이력은 메모리에만 있고 파일을 만들지 않습니다. JSON은 관찰용 표현이며 저장 형식이 아닙니다.

## 화면

- **Current**: 최신 값을 트리로 보여 주고, 바뀐 필드는 색과 이전 값으로 표시합니다.
- **JSON**: 같은 값을 JSON으로 보여 주고 클립보드로 복사합니다.
- **History**: 값이 바뀐 시점의 경로·이전 값·새 값을 보관합니다. Undo 스택은 아닙니다.
- **Pause history / Reset baseline / Clear history**: 이력 표시 정지, 변경 강조 초기화, 이력 비우기. 원본 시스템에는 영향을 주지 않습니다.

## 이전 이름에서 옮기기

이 플러그인의 예전 이름은 `OWTStateMonitor`입니다. `Config/Engine.ini`의 CoreRedirects가 저장된 에셋의 `/Script/OWTStateMonitor` 경로와 `OWTStateMonitorWidget` 클래스를 새 이름으로 연결합니다. C++ 코드는 다음처럼 바꿉니다.

| 이전 | 새 이름 |
|---|---|
| 모듈 `OWTStateMonitor` | `StateMonitor` |
| `FOWTStateMonitorModel`, `FOWTStateMonitor…` | `FStateMonitorModel`, `FStateMonitor…` |
| `SOWTStateMonitor` | `SStateMonitor` |
| `UOWTStateMonitorWidget` | `UStateMonitorWidget` |
| `#include "OWTStateMonitor….h"` | `#include "StateMonitor….h"` |

## 테스트

자동화 테스트는 `StateMonitor.`로 시작합니다(Model, View, Api, Live). 저장소의 `tests/live_probe.py`는 뷰어 대신 47731 포트에서 기다리며 실제 게임이 보낸 hello와 GameMode/GameState/PC0 스냅을 확인합니다. 플러그인만 넣은 빈 프로젝트에서도 같은 테스트가 실행됩니다.
