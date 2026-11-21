<!-- dwocanvas/2 노드 → UMG 위젯. 역할(role)은 v2에서 노드 필드이며, 레이어 이름 접두사는 role이 없을 때의 예비 규칙입니다. -->

# DwoCanvas 매핑표

단계: **N** 기본 위젯 그대로 · **C** 기본 위젯 조합 · **X** 확장 위젯 · **B** 이미지로 굽기 · **U** 미지원(경고만)

### 구조와 레이아웃
| Figma | UMG | 단계 | 상태 |
|---|---|---|---|
| Frame (Auto Layout 없음) | CanvasPanel, 자식은 Constraints → Anchors/Offsets/Alignment | N | 구현 |
| Auto Layout 가로 / 세로 | HorizontalBox / VerticalBox | N | 구현 |
| ″ (LayoutMode = DesignFrame일 때) | `UDwoCanvasFrame` 하나 (배경·패딩·간격·정렬 포함) | X | 구현 |
| Wrap | WrapBox (`InnerSlotPadding` = gap, crossGap) | N | 구현 |
| Grid | GridPanel (columns 기준 행/열) | N | 구현 (span 미지원) |
| Gap / Padding | 슬롯 Padding (Wrap·Grid는 Border Padding) | C | 구현 |
| Space between / center / end | Spacer(Fill) 삽입 | C | 구현 |
| Hug / Fill / Fixed, Min/Max | 슬롯 Auto/Fill, SizeBox Override/Min/Max | N | 구현 |
| Auto Layout 안의 Absolute | Overlay + CanvasPanel | C | 구현 |
| Constraints Left/Right/Center/Stretch/Scale | Canvas 슬롯 | N | 구현 |
| Clip content | Clipping ClipToBounds | N | 구현 |
| Group | 평탄화 (Canvas 안에서는 위젯 없이 자식만 배치) | C | 구현 |
| 회전 / 투명도 / 숨김 | RenderTransform / RenderOpacity / Collapsed | N | 구현 |

### 채우기, 테두리, 효과
| Figma | UMG | 단계 | 상태 |
|---|---|---|---|
| 단색 + 모서리별 radius + Inside 테두리 | Image/Border의 RoundedBox 브러시 | N | 구현 |
| 그라데이션 (선형·방사형·각도형·다이아몬드, 정지점 4개까지) | `UDwoCanvasShape` / `UDwoCanvasFrame` 배경 | X | 구현 |
| Center/Outside 테두리, 점선 | `UDwoCanvasShape` | X | 구현 (점선은 모서리 구간 근사) |
| Drop shadow / Inner shadow (각 1개) | `UDwoCanvasShape` | X | 구현 |
| Background blur | BackgroundBlur | N | 구현 |
| 이미지 채우기 Fill / Fit / Tile | UV 크롭 / ScaleBox / Tiling | N | 구현 (Crop은 가운데 크롭으로 근사) |
| 9-slice | Brush Box + Margin (`images.nineSlice`) | N | 구현 |
| 채우기 여러 겹, 블렌드 모드, Layer blur, 마스크, 벡터, 비원형 타원 | Exporter가 자동으로 굽기 | B | 구현 (Figma 플러그인) |
| Corner smoothing | 일반 radius | U | 정보 |

### 텍스트
| Figma | UMG | 단계 | 상태 |
|---|---|---|---|
| 텍스트 | TextBlock | N | 구현 |
| 폰트 패밀리 / 스타일 | 폰트 규칙(`Fonts`) → UFont + Typeface. 없으면 굵기 추정 | N | 구현 |
| 크기 | px × `FontSizeScale`(0.75) | N | 구현 |
| 줄 높이 / 자간 | `LineHeightPercentage` / `LetterSpacing`(1/1000em) | N | 구현 |
| 정렬, 대소문자 | Justification, TextTransformPolicy | N | 구현 (Justify, Title case 미지원) |
| Auto width / height / fixed / truncate, max lines | 줄바꿈, SizeBox, Ellipsis | N/C | 구현 |
| 텍스트 그림자 | ShadowOffset/Color (blur 무시) | N | 구현 |
| 혼합 스타일, 밑줄·취소선 | RichTextBlock 필요 | C | **미구현** (첫 스타일로 경고) |
| 현지화 | `FText` 키 = `<문서>.<노드id>`, 또는 `db.loc=Table:Key` | C | 구현 |

### 컴포넌트와 디자인 시스템
| Figma | UMG | 단계 | 상태 |
|---|---|---|---|
| Component / Instance | 별도 WBP / 인스턴스 | N | 구현 |
| 오버라이드가 있는 인스턴스 | `InstancePolicy`: 인라인 펼치기(기본) / 참조 | C | 구현 |
| 컴포넌트 → 기존 클래스 (CommonUI 등) | `ComponentOverrides` | N | 구현 |
| 버튼 상태 Variant | FButtonStyle Normal/Hovered/Pressed/Disabled (복잡하면 상태별 굽기) | N/B | 구현 |
| 그 밖의 Variant 전환 | — | X | **미구현** (인스턴스가 쓰는 Variant만 가져옴) |
| Variables / Styles | 색은 `token` → `ColorTokenOverrides`로 덮어쓰기 | C | 일부 (런타임 테마 전환 미구현) |
| 프로토타입 인터랙션·애니메이션 | — | U | 미지원 |


## 규격 v2 역할 이름
| `role` | UMG | 비고 |
|---|---|---|
| `button` | Button (+ 상태별 브러시) | `states.hovered/pressed/disabled` |
| `list` / `tile` | ListView / TileView | 첫 자식 인스턴스의 컴포넌트가 엔트리 WBP |
| `scroll` | ScrollBox | 방향은 layout.mode |
| `textInput` / `checkBox` / `slider` / `progressBar` | EditableTextBox / CheckBox / Slider / ProgressBar | |
| `switcher` / `namedSlot` / `spacer` | WidgetSwitcher / NamedSlot / Spacer | |
| `ignore` | (만들지 않음) | |
| `x-<이름>` | 프로젝트가 등록한 역할 처리기 | 없으면 일반 레이아웃 + 경고 |
