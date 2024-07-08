#include "diagnostics.h"
#include <iostream>

namespace OmegaWrapGen {
   void DiagnosticBuffer::push(Diagnostic *d){
       buffer.push_back(d);
   }

   bool DiagnosticBuffer::hasErrored(){
       for(auto & diag : buffer){
           if(diag->isError()){
               return true;
           };
       };
       return false;
   }

   void DiagnosticBuffer::logAll(){
       while(!buffer.empty()){
           auto di = buffer.front();
           di->format(std::cout);
           std::cout << std::endl;
           buffer.pop_front();
       };
   };

}