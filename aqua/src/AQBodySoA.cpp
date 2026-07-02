#include "AQBodySoA.h"

void AQBodySoA::resize(std::size_t n) {
    posX.resize(n); posY.resize(n); posZ.resize(n);
    velX.resize(n); velY.resize(n); velZ.resize(n);
    quatX.resize(n); quatY.resize(n); quatZ.resize(n); quatW.resize(n);
    wbX.resize(n); wbY.resize(n); wbZ.resize(n);
    invMass.resize(n);
    invInertiaX.resize(n); invInertiaY.resize(n); invInertiaZ.resize(n);
    forceX.resize(n); forceY.resize(n); forceZ.resize(n);
    torqueX.resize(n); torqueY.resize(n); torqueZ.resize(n);
    linearDamping.resize(n); angularDamping.resize(n);
    gravityScale.resize(n); maxAngularSpeed.resize(n);
    comX.resize(n); comY.resize(n); comZ.resize(n);
    pseudoLinX.resize(n); pseudoLinY.resize(n); pseudoLinZ.resize(n);
    activation.resize(n);
}

void AQBodySoA::gatherFrom(const AQBodyState<float>* states, std::size_t n) {
    resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const AQBodyState<float>& s = states[i];
        posX[i] = s.position[0][0]; posY[i] = s.position[1][0]; posZ[i] = s.position[2][0];
        velX[i] = s.velocity[0][0]; velY[i] = s.velocity[1][0]; velZ[i] = s.velocity[2][0];
        quatX[i] = s.orientation.x; quatY[i] = s.orientation.y;
        quatZ[i] = s.orientation.z; quatW[i] = s.orientation.w;
        wbX[i] = s.angularVelBody[0][0]; wbY[i] = s.angularVelBody[1][0]; wbZ[i] = s.angularVelBody[2][0];
        invMass[i] = s.invMass;
        invInertiaX[i] = s.invInertiaBody[0][0];
        invInertiaY[i] = s.invInertiaBody[1][0];
        invInertiaZ[i] = s.invInertiaBody[2][0];
        forceX[i] = s.forceAccum[0][0]; forceY[i] = s.forceAccum[1][0]; forceZ[i] = s.forceAccum[2][0];
        torqueX[i] = s.torqueAccum[0][0]; torqueY[i] = s.torqueAccum[1][0]; torqueZ[i] = s.torqueAccum[2][0];
        linearDamping[i] = s.linearDamping; angularDamping[i] = s.angularDamping;
        gravityScale[i] = s.gravityScale; maxAngularSpeed[i] = s.maxAngularSpeed;
        comX[i] = s.comOffset[0][0]; comY[i] = s.comOffset[1][0]; comZ[i] = s.comOffset[2][0];
        // Pseudo-velocity is per-substep solver state, not AQBodyState — a
        // fresh gather starts it at zero (the "no solver ran" value).
        pseudoLinX[i] = 0.f; pseudoLinY[i] = 0.f; pseudoLinZ[i] = 0.f;
        activation[i] = static_cast<std::uint8_t>(s.activation);
    }
}

void AQBodySoA::scatterTo(AQBodyState<float>* states, std::size_t n) const {
    const std::size_t count = (n < size()) ? n : size();
    for (std::size_t i = 0; i < count; ++i) {
        AQBodyState<float>& s = states[i];
        s.position[0][0] = posX[i]; s.position[1][0] = posY[i]; s.position[2][0] = posZ[i];
        s.velocity[0][0] = velX[i]; s.velocity[1][0] = velY[i]; s.velocity[2][0] = velZ[i];
        s.orientation.x = quatX[i]; s.orientation.y = quatY[i];
        s.orientation.z = quatZ[i]; s.orientation.w = quatW[i];
        s.angularVelBody[0][0] = wbX[i]; s.angularVelBody[1][0] = wbY[i]; s.angularVelBody[2][0] = wbZ[i];
        s.invMass = invMass[i];
        s.invInertiaBody[0][0] = invInertiaX[i];
        s.invInertiaBody[1][0] = invInertiaY[i];
        s.invInertiaBody[2][0] = invInertiaZ[i];
        s.forceAccum[0][0] = forceX[i]; s.forceAccum[1][0] = forceY[i]; s.forceAccum[2][0] = forceZ[i];
        s.torqueAccum[0][0] = torqueX[i]; s.torqueAccum[1][0] = torqueY[i]; s.torqueAccum[2][0] = torqueZ[i];
        s.linearDamping = linearDamping[i]; s.angularDamping = angularDamping[i];
        s.gravityScale = gravityScale[i]; s.maxAngularSpeed = maxAngularSpeed[i];
        s.comOffset[0][0] = comX[i]; s.comOffset[1][0] = comY[i]; s.comOffset[2][0] = comZ[i];
        s.activation = static_cast<AQActivationState>(activation[i]);
    }
}
