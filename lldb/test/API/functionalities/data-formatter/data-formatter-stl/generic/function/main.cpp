#include <functional>

// Helper types

struct IntBox {
  int value;

  /* implicit */ IntBox(int value) : value(value) {}
};

template <typename A, typename B> struct Pair {};

// std::function targets

void Free(int) {
  // freeBody
}
void FreeConverting(IntBox) {
  // freeConvertingBody
}

struct Base {
  void Member(int) {
    // memberBody
  }
  void MemberConverting(IntBox) {
    // memberConvertingBody
  }

  virtual void MemberVirtual(int) {}
};

struct Callable {
  void operator()(int) {
    // callableBody
  }
};
struct CallableConverting {
  void operator()(IntBox) {
    // callableConvertingBody
  }
};
struct CallableOverloaded {
  void operator()(int) {
    // callableOverloadedIntBody
  }
  void operator()(IntBox) {
    // callableOverloadedIntBoxBody
  }
};

int main() {
  // Normal cases
  std::function<void(int)> free = Free;
  std::function<void(int)> callable = Callable();
  std::function<void(int)> lambda = [](int) {
    // lambdaBody
  };
  std::function<void(Base &, int)> member = &Base::Member;

  // IntBox conversion occurs in __func<...>::operator()
  std::function<void(int)> freeConverting = FreeConverting;
  std::function<void(int)> callableConverting = CallableConverting();
  std::function<void(int)> lambdaConverting = [](IntBox) {
    // lambdaConvertingBody
  };
  std::function<void(Base &, int)> memberConverting = &Base::MemberConverting;

  // Lambda's typename is complex
  std::function<void(int)> lambdaComplex = [](Pair<int, Pair<int, int>> = {}) {
    return [](int) {
      // lambdaComplexBody
    };
  }();

  // Callable has overloaded operator()
  std::function<void(int)> callableOverloadedInt = CallableOverloaded();
  std::function<void(IntBox)> callableOverloadedIntBox = CallableOverloaded();

  // Member function is virtual
  std::function<void(Base &, int)> memberVirtual = &Base::MemberVirtual;

#if __has_feature(blocks)
  // Block support
  std::function<void(int)> block = ^void(int) {
    // blockBody
  };
  std::function<void(int)> blockConverting = ^void(IntBox) {
    // blockConvertingBody
  };
#endif

  Free(0); // break 1
  Free(0); // break 2

  return 0;
}
