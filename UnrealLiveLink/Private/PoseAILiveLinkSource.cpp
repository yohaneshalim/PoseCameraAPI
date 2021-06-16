// Copyright Pose AI Ltd 2021

#include "PoseAILiveLinkSource.h"
#include "ILiveLinkClient.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "HAL/RunnableThread.h"
#include "LiveLinkLog.h"


static int lockedAt;
static int unlockedAt;
static FCriticalSection mutx; 


PoseAILiveLinkSource::PoseAILiveLinkSource(int32 portNum, const PoseAIHandshake& handshake, bool useRootMotion) :
	port(portNum), handshake(handshake), useRootMotion(useRootMotion), enabled(true), status(LOCTEXT("statusConnecting", "connecting"))
{
	UE_LOG(LogTemp, Display, TEXT("PoseAI: connecting to %d"), portNum);
	usedPorts.Emplace(portNum);
	udpServer = MakeShared<PoseAILiveLinkServer, ESPMode::ThreadSafe>();
	udpServer->CreateServer(portNum, handshake);
	udpServer->OnPoseReceived().BindRaw(this, &PoseAILiveLinkSource::UpdatePose);
	FString myIP;
	udpServer->GetIP(myIP);
	status = FText::FormatOrdered(LOCTEXT("statusConnected", "listening on {0} Port:{1}"), FText::FromString(myIP), FText::FromString(FString::FromInt(portNum)));
	
}

PoseAIRig PoseAILiveLinkSource::MakeRig() {
	return PoseAIRig(
		FName(handshake.rig),
		useRootMotion,
		!handshake.mode.Contains(TEXT("BodyOnly")), //includeHands
		handshake.isMirrored,
		handshake.mode.Contains(TEXT("Desktop")) //isDesktop
		);
}

void PoseAILiveLinkSource::AddSubject(FName name)
{
	check(IsInGameThread());
	FLiveLinkSubjectPreset subject;
	subject.bEnabled = true;
	subject.Key = FLiveLinkSubjectKey(sourceGuid, name);
	subject.Role = TSubclassOf<ULiveLinkRole>(ULiveLinkAnimationRole::StaticClass());
	subject.Settings = nullptr;
	subject.VirtualSubject = nullptr;
	
	mutx.Lock(); lockedAt = __LINE__;
	if (subjectKeys.Find(name) != 0) {
		UE_LOG(LogTemp, Display, TEXT("PoseAILiveLink: replacing %s with new connection"), *(name.ToString()));
		liveLinkClient->RemoveSubject_AnyThread(subjectKeys[name]);
		subjectKeys.Remove(name);
	}
	UE_LOG(LogTemp, Display, TEXT("PoseAIiveLink: adding %s to subjects"), *(name.ToString()));
	if (!liveLinkClient->CreateSubject(subject)) {
		UE_LOG(LogTemp, Warning, TEXT("PoseAILiveLink: unable to create subject %s"), *(name.ToString()));
		return;
	}

	rigs.Add(name, MakeRig());
	FLiveLinkStaticDataStruct rigDefinition = rigs[name].MakeStaticData(); 
	liveLinkClient->RemoveSubject_AnyThread(subject.Key);
	liveLinkClient->PushSubjectStaticData_AnyThread(subject.Key, ULiveLinkAnimationRole::StaticClass(), MoveTemp(rigDefinition));
	subjectKeys.Add(name, subject.Key);
	mutx.Unlock(); unlockedAt = __LINE__;
}

void PoseAILiveLinkSource::Update()
{
	while (!newConnections.IsEmpty())
	{
		FName newSubject;
		newConnections.Dequeue(newSubject);
		AddSubject(newSubject);
	}
	
}

TArray<int32> PoseAILiveLinkSource::usedPorts = {};

bool PoseAILiveLinkSource::IsValidPort(int32 port) {
	return !usedPorts.Contains(port);
}

bool PoseAILiveLinkSource::IsSourceStillValid() const
{
	return true;
}

PoseAILiveLinkSource::~PoseAILiveLinkSource()
{
	usedPorts.Remove(port);
	UE_LOG(LogTemp, Display, TEXT("PoseAI LiveLink: PoseAILiveLinkSource on port %d closed"), port);

	
}

void PoseAILiveLinkSource::ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid)
{
	sourceGuid = InSourceGuid;
	client = InClient;
	liveLinkClient = InClient;

	UE_LOG(LogTemp, Display, TEXT("Pose AI LiveLink: receive client %s"), *client->ModularFeatureName.ToString());
	for (auto& elem : subjectKeys) {
		AddSubject(elem.Key);
	}
 }

void PoseAILiveLinkSource::disable()
{
	UE_LOG(LogTemp, Display, TEXT("Pose AI LiveLink: disabling the source"));
	status = LOCTEXT("statusDisabled", "disabled");

	mutx.Lock(); lockedAt = __LINE__;
	liveLinkClient = nullptr;
	mutx.Unlock(); unlockedAt = __LINE__;

	enabled = false;
}


bool PoseAILiveLinkSource::RequestSourceShutdown()
{
	UE_LOG(LogTemp, Display, TEXT("PoseAI LiveLink: requested source shutdown"));
	if (liveLinkClient != nullptr) {
		for (auto& elem : subjectKeys) {
			liveLinkClient->RemoveSubject_AnyThread(elem.Value);
			UE_LOG(LogTemp, Display, TEXT("PoseAI LiveLink: removing subject %s"), *(elem.Key.ToString()));
		}
	}
	subjectKeys.Empty();
	if (udpServer)
		udpServer->CleanUp();
	udpServer = nullptr;

	mutx.Lock(); lockedAt = __LINE__;
	liveLinkClient = nullptr;
	mutx.Unlock(); unlockedAt = __LINE__;
	return true;
}

void PoseAILiveLinkSource::UpdatePose(FName& name, TSharedPtr<FJsonObject> jsonPose)
{
	if (liveLinkClient == nullptr)
		return;
	if (subjectKeys.Find(name) == nullptr) {
		UE_LOG(LogTemp, Display, TEXT("PoseAILiveLink: cannot find %s to update frame.  Adding new subject."), *(name.ToString()));
		newConnections.Enqueue(name);
		return;
	}
	PoseAIRig* rig = rigs.Find(name);
	FLiveLinkFrameDataStruct frameData(FLiveLinkAnimationFrameData::StaticStruct());
	if (rig != nullptr && rig->ProcessFrame(jsonPose, frameData)) {
		liveLinkClient->PushSubjectFrameData_AnyThread(subjectKeys[name], MoveTemp(frameData));
	}
}
