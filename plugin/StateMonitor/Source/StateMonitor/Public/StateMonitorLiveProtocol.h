#pragma once

#include "CoreMinimal.h"

struct FStateMonitorSource;

/** @brief 연결 직후 게임이 뷰어에 보내는 hello 메시지의 내용. */
struct STATEMONITOR_API FStateMonitorLiveHello
{
	FString Project;
	FString World;
	/** @brief Standalone, ListenServer, Client, DedicatedServer 중 하나. */
	FString NetMode;
	FString EngineVersion;
	FString BuildConfiguration;
	double TimeSeconds = 0.0;
	uint32 ProcessId = 0;
	/** @brief PIE 인스턴스 번호. PIE가 아니면 -1. */
	int32 PieInstance = -1;
};

enum class EStateMonitorLiveCommand : uint8
{
	Unknown,
	Auto,
	Watch,
	Unwatch,
	Pause,
	Resync
};

/** @brief 뷰어가 보낸 명령 한 줄을 해석한 결과. 메시지에 없던 선택 필드는 bHas…가 false다. */
struct STATEMONITOR_API FStateMonitorLiveCommand
{
	EStateMonitorLiveCommand Type = EStateMonitorLiveCommand::Unknown;
	FString ClassName;
	double Interval = 0.25;
	int32 Max = 16;
	bool bOn = false;
	bool bIncludeEngineProperties = false;
	bool bHasInterval = false;
	bool bHasEngine = false;
};

/**
 * @brief dwo-Lens 뷰어와 주고받는 Live 프로토콜 v1(UTF-8 JSON Lines).
 * 모든 함수는 소켓 없이 호출할 수 있는 순수 함수이며, 반환한 줄에는 끝의 '\n'이 없다.
 */
namespace StateMonitorLive
{
constexpr int32 ProtocolVersion = 1;
constexpr int32 DefaultPort = 47731;

/** @brief {"t":"hello",...} */
STATEMONITOR_API FString MakeHelloLine(const FStateMonitorLiveHello& Hello);

/** @brief {"t":"world","world":"..."} */
STATEMONITOR_API FString MakeWorldLine(const FString& World);

/** @brief {"t":"snap",...}. value에는 Source.CurrentJson을 한 줄로 줄여 그대로 넣는다. */
STATEMONITOR_API FString MakeSnapLine(const FStateMonitorSource& Source, bool bAuto);

/** @brief {"t":"remove","id":"..."} */
STATEMONITOR_API FString MakeRemoveLine(FName SourceId);

/** @brief 모델이 만든 들여쓴 JSON에서 줄바꿈과 들여쓰기를 지운다. 문자열 안의 줄바꿈은 이미 이스케이프되어 있다. */
STATEMONITOR_API FString CompactJson(const FString& Json);

/** @brief 뷰어 명령 한 줄을 해석한다. JSON이 아니거나 알 수 없는 명령이면 false. */
STATEMONITOR_API bool ParseCommand(const FString& Line, FStateMonitorLiveCommand& OutCommand);
} // namespace StateMonitorLive
