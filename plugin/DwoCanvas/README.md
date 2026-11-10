# DwoCanvas

dwo-Forge가 제공하는 도구 중 하나입니다. **정해진 시안 규격(dwocanvas/2 JSON)**을 받아 언리얼 위젯 블루프린트(WBP)의 디자이너 트리를 만듭니다.

시안이 어디서 왔는지는 플러그인이 알지 못합니다. 원본 도구마다 변환기(어댑터)가 규격 JSON을 만들고, 플러그인은 그 규격만 읽습니다.

```
[원본]      Figma   HTML/CSS   이미지·텍스트·낙서(추론)   직접 작성
[변환기]    canvas/adapters/figma · html · inference
              ▼  dwocanvas/2 JSON (Schema/dwocanvas.schema.json)
[검증]      canvas/python  (python -m dwocanvas validate)   ← 변환기·CI에서 사용
              ▼
[이 플러그인] v1 자동 변환 → 스키마 검증 → 토큰 적용 → 역할 처리기 → 위젯 트리
            → 재임포트 보존 · C++ 바인딩 검증 · 시안 비교 · 결과 창
```

## 구성
| 위치 | 내용 |
|---|---|
| `Source/DwoCanvasRuntime` | 게임에 포함되는 확장 위젯: `UDwoCanvasShape`, `UDwoCanvasFrame`, `FDwoCanvasShapeStyle` |
| `Source/DwoCanvasEditor` | 임포터, 스키마 검증(`FDwoCanvasSchema`), 역할 처리기(`IDwoCanvasRoleHandler`), 설정, 재임포트 기록, 시안 비교, 결과 창, 메뉴·Python·커맨드릿, 자동화 테스트 |
| `Content/Materials/M_DwoCanvasShape` | 그라데이션·테두리·점선·그림자용 UI 머티리얼 (`Tools/create_shape_material.py`로 재생성) |
| `Schema/dwocanvas.schema.json` | 현재 규격 (v2). `designbridge-1.schema.json`은 이전 규격 |
| `Config/DefaultDwoCanvas.ini` | 이전 이름(DesignBridge)으로 저장된 에셋을 위한 리다이렉트 |
| `Samples/` | 예제 시안 (v1: `sample_shop.json`, `DwoMockup.dwocanvas.json` / v2: `sample_settings.dwocanvas.json`) |
| `Conformance/` | 적합성 케이스: 원본 도구의 렌더 이미지와 UE 렌더를 비교 |

## 설치와 사용
- dwo-Forge로 설치합니다: `dwo-Forge.bat canvas-install <프로젝트>`. 원본은 dwo-Forge에 있고, 플러그인은 `Plugins/__DwoCanvas`에 복사됩니다(git에서 숨김).
  - `canvas-remove`나 `uninstall` 때 지워집니다. 생성된 위젯이 런타임 모듈을 쓰고 있으면 제거를 멈추고 알려 줍니다.
  - 생성된 위젯을 여는 다른 PC에도 DwoCanvas가 설치되어 있어야 합니다.
- **가져오기 방법**
  - 에디터: **Tools > DwoCanvas > Import Design File...** (`*.dwocanvas.json`)
  - 명령줄: `UnrealEditor-Cmd.exe <프로젝트> -run=DwoCanvasImport -File=<json> [-ResetOverrides] [-Doc=이름]` (오류가 없으면 종료 코드 0)
  - Python: `unreal.DwoCanvasLibrary.import_design_file(path, False)`
  - dwo-Forge: `canvas-import <파일>`, `canvas-validate <파일>`
- 결과는 **결과 창**에 표시되고, 리포트 파일은 `Saved/DwoCanvas/Reports/`에 저장됩니다.
  - 결과 창 내용: 상태, 변경 수, 만들어진 WBP, C++ 연결, 시안 일치도, 확인할 항목
- 재임포트 기록 `DwoCanvas/Records/`는 WBP와 함께 커밋합니다. 이전 `DesignBridge/Records/`도 그대로 읽습니다.

## 규격 v2에서 바뀐 점
| 항목 | 내용 |
|---|---|
| `role`, `variable`, `widgetName` | 노드의 정식 필드입니다. 변환기가 채웁니다. 레이어 이름 접두사(`btn_` 등)는 `role`이 없을 때만 쓰는 예비 규칙입니다 |
| `capabilities` | 원본이 제공한 정보 목록입니다. `stable-ids`가 없으면 재임포트 보존이 보장되지 않는다고 알리고, `inferred`가 있으면 초안으로 처리합니다 |
| `source` | `tool`, `adapter`, `mode`(`extract` 또는 `draft`), `model`. 결과 창과 리포트에 표시됩니다 |
| `tokens` | W3C Design Tokens 형식입니다. 색(`colorToken`)과 숫자(노드의 `tokens`: gap, crossGap, padding, radius, strokeWidth, opacity, fontSize)를 지원하고, `{별칭}` 참조도 지원합니다. 프로젝트 설정의 `ColorTokenOverrides` / `NumberTokenOverrides`가 우선합니다 |
| `assets` | 이전의 `images`입니다. `data`(base64) 또는 `uri`(상대 경로·`file://`), 그리고 `sha256`과 `scale`, `nineSlice` |
| 글꼴 | `weight`(100~900)와 `italic`. 폰트 규칙에서 `TypefaceByWeight`로 서체를 지정합니다 |
| `confidence`, `inferred` | 추론 노드의 확신도와 추정한 필드입니다. 기준값(`LowConfidenceThreshold`, 0.6)보다 낮으면 확인 항목으로 올립니다 |
| `x-*` | 도구별 확장 필드입니다. 무시하고 리포트에 남깁니다 |

- `designbridge/1` 파일은 불러올 때 v2로 자동 변환합니다.
- 규격 위반은 경로와 함께 오류로 알리고 가져오지 않습니다. 설정 `bStrictSchema`를 끄면 경고만 내고 가져옵니다.

## 추론 초안 (`source.mode: "draft"`)
이미지·텍스트·낙서에서 추론해 만든 문서입니다.
- 결과는 `DraftFolder`(기본 `/Game/UI/Drafts`)에 **매번 새 WBP**로 만듭니다(`WBP_이름`, `_2`, `_3`...).
- 재임포트 기록을 남기지 않습니다.
- 권장 흐름: 초안을 검토한 뒤 디자인 도구(Figma)로 옮겨 다듬습니다. 그 뒤로는 일반적인 추출 흐름으로 가져옵니다.

## 역할 처리기 (프로젝트 확장)
프로젝트 에디터 모듈에서 역할별로 위젯을 직접 만들 수 있습니다. 두 가지로 씁니다.
- **확장 역할:** `x-이름` 역할에 프로젝트 위젯을 연결합니다.
- **기본 역할 교체:** 예를 들어 `button`을 CommonUI 버튼으로 만듭니다.

```cpp
class FInventorySlotHandler : public IDwoCanvasRoleHandler
{
public:
	virtual FString GetRole() const override { return TEXT("x-inventorySlot"); }
	virtual UWidget* Build(FDwoCanvasBuildContext& Context) override
	{
		bool bKeptEdited = false;
		UMySlotWidget* Slot = Context.MakeWidget<UMySlotWidget>(TEXT(""), bKeptEdited);  // 재임포트 보존에 참여
		if (Slot && !bKeptEdited) { /* Context.GetNode()의 값 적용 */ }
		return Slot;  // nullptr이면 기본 매핑으로 처리
	}
};

// StartupModule
FDwoCanvasRoleHandlers::Register(MakeShared<FInventorySlotHandler>());
```
- `Context.BuildChildren(Panel)`: 자식 노드를 패널 종류에 맞는 슬롯 규칙으로 만듭니다.
- `Context.ResolveColor(Paint)`: 토큰을 반영한 색을 돌려줍니다.
- `Context.Info` / `Context.Warn`: 리포트에 남깁니다.

## 매핑표
노드 종류별로 어떤 UMG 위젯이 되는지는 [canvas/docs/SchemaFirstArchitecture.md](../../canvas/docs/SchemaFirstArchitecture.md)와 [canvas/docs/MappingTable.md](../../canvas/docs/MappingTable.md)에 있습니다.

## 테스트
```
UnrealEditor-Cmd.exe <프로젝트> -ExecCmds="Automation RunTests DwoCanvas;Quit" -unattended -nosplash
```
| 테스트 | 확인 내용 |
|---|---|
| `Import.Sample` / `Render.Sample` / `Import.Fidelity` | 매핑, 렌더, Figma와 맞춘 세부 표현 |
| `Import.Reimport` / `Import.ExistingContentGuard` | UE 수정 보존, 사용자 추가 위젯, 직접 만든 WBP 보호 |
| `Import.CppParentBindings` / `Import.DwoCanvasFrameMode` | C++ 부모 바인딩, DwoCanvasFrame 모드 |
| `Schema.Validation` / `Schema.Migration` | 규격 위반 거부(경로 포함), v1 → v2 변환 |
| `Import.V2Roles` / `Import.Tokens` / `Import.RoleHandler` / `Import.InferredDraft` | 역할 필드, 토큰과 설정 우선순위, 처리기, 초안 |
| `Import.AllSamples` / `Conformance` | 모든 예제, 원본 렌더와의 일치도 |

## 알려진 한계
- 텍스트 혼합 스타일과 밑줄·취소선은 RichText가 필요해 아직 지원하지 않습니다(첫 스타일로 처리).
- 버튼 상태 외의 Variant 전환과 실행 중 테마 전환은 지원하지 않습니다. 토큰은 가져올 때 값으로 반영됩니다.
- 시안 비교는 에디터에서 가져올 때만 동작합니다(커맨드릿에는 렌더러가 없음).
