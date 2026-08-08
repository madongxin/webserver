#pragma once

#include "auth.pb.h"

class AuthServiceImpl : public auth::AuthService {
public:
    void Login(::google::protobuf::RpcController *controller, const ::auth::LoginRequest *request,
               ::auth::LoginResponse *response, ::google::protobuf::Closure *done) override;
    void Register(::google::protobuf::RpcController *controller,
                  const ::auth::RegisterRequest *request, ::auth::RegisterResponse *response,
                  ::google::protobuf::Closure *done) override;
    void VerifyToken(::google::protobuf::RpcController *controller,
                     const ::auth::VerifyTokenRequest *request,
                     ::auth::VerifyTokenResponse *response,
                     ::google::protobuf::Closure *done) override;
    void RefreshToken(::google::protobuf::RpcController *controller,
                      const ::auth::RefreshTokenRequest *request,
                      ::auth::RefreshTokenResponse *response,
                      ::google::protobuf::Closure *done) override;
};
