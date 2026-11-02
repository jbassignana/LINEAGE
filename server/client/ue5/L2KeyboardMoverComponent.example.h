// EXAMPLE glue for the UE5 client (L2Net plugin). Not compiled here: no
// Unreal in this environment. It shows where L2KeyboardMover plugs in.
//
// 1. Project Settings > Input: axis "MoveForward" (W=+1, S=-1),
//    "MoveRight" (D=+1, A=-1). Enhanced Input works the same way.
// 2. Add this component to the local player's pawn.
// 3. Make WASD the default: in the options menu, "Movement: Keyboard (WASD)"
//    on by default; click-to-move still works (it sends movement=1).
//
// Coordinates: L2 uses x east, y south, z up, 1 unit ~ 1 UE unit if you
// import the world at that scale. Heading 0 = +x.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "L2KeyboardMover.h"
#include "L2NetSubsystem.h"            // from the L2Net plugin
#include "L2KeyboardMoverComponent.generated.h"

UCLASS(ClassGroup = (L2), meta = (BlueprintSpawnableComponent))
class UL2KeyboardMoverComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UL2KeyboardMoverComponent() { PrimaryComponentTick.bCanEverTick = true; }

    // call from the pawn's input bindings.
    void SetMoveInput(float Forward, float Right) { InForward = Forward; InRight = Right; }

    // call when UserInfo arrives (spawn) and when run speed changes.
    void ResetTo(const FVector& L2Pos, float RunSpeed)
    {
        Mover.set_speed(RunSpeed);
        Mover.set_position({L2Pos.X, L2Pos.Y, L2Pos.Z});
    }

    // call when the server sends ValidateLocation (0x61) for our own object.
    void OnServerPosition(const FVector& L2Pos) { Mover.on_server_position({L2Pos.X, L2Pos.Y, L2Pos.Z}); }

    virtual void TickComponent(float Dt, ELevelTick, FActorComponentTickFunction*) override
    {
        const APlayerController* PC = GetWorld()->GetFirstPlayerController();
        const double Yaw = FMath::DegreesToRadians(PC ? PC->GetControlRotation().Yaw : 0.0);
        Mover.set_input(InForward, InRight, Yaw);

        l2::Packet Out[2];
        const int N = Mover.tick(GetWorld()->GetTimeSeconds() * 1000.0, Dt, Out);
        for (int i = 0; i < N; ++i)
            Send(Out[i]);

        // the pawn follows the prediction every frame (no waiting for the echo).
        const l2::Vec3 P = Mover.position();
        GetOwner()->SetActorLocation(FVector(P.x, P.y, P.z));
        if (Mover.moving())
            GetOwner()->SetActorRotation(FRotator(0, Mover.heading() * 360.0 / 65536.0, 0));
    }

private:
    void Send(const l2::Packet& P)
    {
        UL2NetSubsystem* Net = GetWorld()->GetGameInstance()->GetSubsystem<UL2NetSubsystem>();
        TArray<uint8> B;
        auto D = [&B](int32 V) { B.Append(reinterpret_cast<uint8*>(&V), 4); };
        switch (P.kind)
        {
        case l2::PacketKind::Move:      // MoveBackwardToLocation, movement = 0 (keyboard)
            B.Add(0x01); D(P.tx); D(P.ty); D(P.tz); D(P.x); D(P.y); D(P.z); D(0);
            break;
        case l2::PacketKind::Validate:  // ValidatePosition
            B.Add(0x48); D(P.x); D(P.y); D(P.z); D(P.heading); D(0);
            break;
        case l2::PacketKind::Stop:      // CannotMoveAnymore
            B.Add(0x36); D(P.x); D(P.y); D(P.z); D(P.heading);
            break;
        }
        Net->SendGamePacket(B);
    }

    l2::KeyboardMover Mover;
    float InForward = 0.f;
    float InRight = 0.f;
};
