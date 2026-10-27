# State Monitor

## 큰 그림

USTRUCT 값을 제출하면 필드 트리, JSON, 변경 이력을 보여 주는 런타임 읽기 전용 모니터입니다. 다른 플러그인이나 프로젝트 코드에 의존하지 않으므로 `Plugins/StateMonitor` 폴더를 어느 UE 5.7 프로젝트의 `Plugins`에 복사해도 바로 쓸 수 있습니다.

```cpp
// 어디서든 한 줄로 제출하고, 한 줄로 화면에 띄운다.
UStateMonitorSubsystem::Get(this)->Submit(TEXT("Player"), PlayerState);
UStateMonitorSubsystem::Get(this)->ShowMonitor();
```

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
- `USTRUCT` 안의 `UPROPERTY`만 읽습니다. 객체 참조는 경로나 null로 표시하고, 참조 대상 내부를 따라가지 않습니다.
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

자동화 테스트는 `StateMonitor.`로 시작합니다(Model, View, Api). 플러그인만 넣은 빈 프로젝트에서도 같은 테스트가 실행됩니다.
