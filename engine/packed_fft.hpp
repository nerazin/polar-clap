#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>
#include <vector>

namespace polar_recovered {
// Portable packed real-FFT service. Forward is normalized by N, inverse is
// its complementary unnormalized real synthesis. DC/Nyquist share pair0.
// This adapter is explicit because the original SDK service isn't supplied.
class PackedRealFFT {
    std::vector<std::complex<double>> work_;
    void transform(int n,bool inverse) {
        for(int i=1,j=0;i<n;i++) {
            int bit=n>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;
            if(i<j)std::swap(work_[i],work_[j]);
        }
        for(int length=2;length<=n;length<<=1) {
            double a=(inverse?2.0:-2.0)*3.1415926535897932384626433832795/length;
            std::complex<double> root(std::cos(a),std::sin(a));
            for(int start=0;start<n;start+=length) {
                std::complex<double> w(1,0);
                for(int j=0;j<length/2;j++) {
                    auto u=work_[start+j],v=work_[start+j+length/2]*w;
                    work_[start+j]=u+v;work_[start+j+length/2]=u-v;w*=root;
                }
            }
        }
    }
public:
    explicit PackedRealFFT(int maximum=32768):work_(maximum){}
    void forward(float* x,int order) {
        int n=1<<order;if(n>int(work_.size()))throw std::length_error("FFT capacity");
        for(int i=0;i<n;i++)work_[i]={x[i],0};
        transform(n,false);
        x[0]=float(work_[0].real()/n);x[1]=float(work_[n/2].real()/n);
        for(int k=1;k<n/2;k++){x[2*k]=float(work_[k].real()/n);x[2*k+1]=float(work_[k].imag()/n);}
    }
    void inverse(float* x,int order) {
        int n=1<<order;if(n>int(work_.size()))throw std::length_error("FFT capacity");
        work_[0]={x[0],0};work_[n/2]={x[1],0};
        for(int k=1;k<n/2;k++){work_[k]={x[2*k],x[2*k+1]};work_[n-k]=std::conj(work_[k]);}
        transform(n,true);for(int i=0;i<n;i++)x[i]=float(work_[i].real());
    }
};
}
