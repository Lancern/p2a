//
// Created by Sirui Mu on 2020/12/26.
//

#pragma clang diagnostic push
#pragma ide diagnostic ignored "OCUnusedGlobalDeclarationInspection"

#ifndef LLVM_ANDERSON_POINTS_TO_ANALYSIS_H
#define LLVM_ANDERSON_POINTS_TO_ANALYSIS_H

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <llvm/ADT/iterator_range.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/Pass.h>
#include <llvm/Support/Casting.h>

#define NON_COPIABLE_NON_MOVABLE(className)             \
  className(const className &) = delete;                \
  className(className &&) noexcept = delete;            \
  className& operator=(const className &) = delete;     \
  className& operator=(className &&) noexcept = delete;

namespace llvm {

namespace anderson {

namespace details {

template <typename T>
struct PolymorphicHasher {
  size_t operator()(const T &obj) const noexcept {
    return obj.GetHashCode();
  }
};

template <typename Container, typename K>
using IndexResultType = decltype(std::declval<Container>()[std::declval<K>()]);

template <typename Container, typename K>
auto find_in(Container &container, const K &key) noexcept -> std::remove_reference_t<IndexResultType<Container, K>> * {
  auto it = container.find(key);
  if (it == container.end()) {
    return nullptr;
  }
  return &it->second;
}

} // namespace details

class Pointee;
class Pointer;
class ValueTreeNode;

/**
 * Different kinds of pointer assignment statements.
 */
enum class PointerAssignmentKind {
  /**
   * Pointer assignment statement of the form `p = &q[...]`.
   *
   * Note that `p = q` can be regarded as a special case of this pointer assignment form since it's equivalent to
   * `p = &q[0]`.
   */
  AssignedElementPtr,

  /**
   * Pointer assignment statement of the form `p = *q`.
   */
  AssignedPointee,

  /**
   * Pointer assignment statement of the form `*p = q`.
   */
  PointeeAssigned,
};

/**
 * Base class of pointer assignment statements.
 */
class PointerAssignment {
public:
  /**
   * Get the kind of this pointer assignment statement.
   *
   * @return the kind of this pointer assignment statement.
   */
  PointerAssignmentKind kind() const noexcept { return _kind; }

  /**
   * Get the pointer operand on the right hand side of this pointer assignment statement.
   *
   * @return the pointer operand on the right hand side of this pointer assignment statement.
   */
  Pointer* pointer() noexcept { return _pointer; }

  /**
   * Get the pointer operand on the right hand side of this pointer assignment statement.
   *
   * @return the pointer operand on the right hand side of this pointer assignment statement.
   */
  const Pointer* pointer() const noexcept { return _pointer; }

  /**
   * Get the hash code of this PointerAssignment object.
   *
   * @return the hash code of this PointerAssignment object.
   */
  virtual size_t GetHashCode() const noexcept;

  virtual bool operator==(const PointerAssignment &rhs) const noexcept;

  virtual bool operator!=(const PointerAssignment &rhs) const noexcept {
    return !operator==(rhs);
  }

protected:
  /**
   * Construct a new PointerAssignment object.
   *
   * @param kind kind of the pointer assignment statement.
   * @param pointer the pointer operand on the right hand side of this assignment.
   */
  explicit PointerAssignment(PointerAssignmentKind kind, Pointer *pointer) noexcept
    : _kind(kind),
      _pointer(pointer)
  {
    assert(pointer && "pointer cannot be null");
  }

  virtual ~PointerAssignment() noexcept = default;

private:
  PointerAssignmentKind _kind;
  Pointer *_pointer;
};

/**
 * Represent a pointer index operand.
 */
class PointerIndex {
public:
  /**
   * A reserved pointer index value that represents the index is calculated dynamically.
   */
  constexpr static const size_t DynamicIndex = static_cast<size_t>(-1);

  /**
   * Construct a new PointerIndex object that represents a dynamically calculated pointer index operand.
   */
  explicit PointerIndex() noexcept
    : _index(DynamicIndex)
  { }

  /**
   * Construct a new PointerIndex object that represents a compile-time constant pointer index operand.
   *
   * @param index the constant index.
   */
  explicit PointerIndex(size_t index) noexcept
    : _index(index)
  { }

  /**
   * Get the compile-time constant pointer index.
   *
   * @return the compile-time constant pointer index. If this PointerIndex object does not represent a compile-time
   * constant pointer index, returns `DynamicIndex`.
   */
  size_t index() const noexcept { return _index; }

  /**
   * Determine whether this PointerIndex object represents a compile-time constant pointer index.
   *
   * @return whether this PointerIndex object represents a compile-time constant pointer index.
   */
  bool isConstant() const noexcept { return _index != DynamicIndex; }

  /**
   * Determine whether this PointerIndex object represents a dynamically computed pointer index.
   *
   * @return whether this PointerIndex object represents a dynamically computed pointer index.
   */
  bool isDynamic() const noexcept { return _index == DynamicIndex; }

  bool operator==(const PointerIndex &rhs) const noexcept {
    return _index == rhs._index;
  }

  bool operator!=(const PointerIndex &rhs) const noexcept {
    return _index != rhs._index;
  }

private:
  size_t _index;
};

/**
 * Represents a pointer assignment statement of the form `p = &q[...]`.
 */
class PointerAssignedElementPtr : public PointerAssignment {
public:
  static bool classof(const PointerAssignment *obj) noexcept {
    return obj->kind() == PointerAssignmentKind::AssignedElementPtr;
  }

  /**
   * Construct a new PointerAssignedElementPtr object.
   *
   * @param pointer the pointer operand on the right hand side of the pointer assignment statement.
   * @param indexSequence the sequence of pointer index.
   */
  explicit PointerAssignedElementPtr(Pointer *pointer, std::vector<PointerIndex> indexSequence) noexcept
    : PointerAssignment {PointerAssignmentKind::AssignedElementPtr, pointer },
      _indexSequence(std::move(indexSequence))
  { }

  /**
   * Get an iterator range that contains the sequence of pointer indexes in this pointer assignment statement.
   *
   * @return an iterator range that contains the sequence of pointer indexes in this pointer assignment statement.
   */
  llvm::iterator_range<typename std::vector<PointerIndex>::const_iterator> index_sequence() const noexcept {
    return llvm::iterator_range<typename std::vector<PointerIndex>::const_iterator> {
      _indexSequence.cbegin(),
      _indexSequence.cend()
    };
  }

  size_t GetHashCode() const noexcept final;

  bool operator==(const PointerAssignment &rhs) const noexcept final;

private:
  std::vector<PointerIndex> _indexSequence;
};

/**
 * Represent a pointer assignment statement of the form `p = *q`.
 */
class PointerAssignedPointee : public PointerAssignment {
public:
  static bool classof(const PointerAssignment *obj) noexcept {
    return obj->kind() == PointerAssignmentKind::AssignedPointee;
  }

  /**
   * Construct a new PointerAssignedPointee object.
   *
   * @param pointer the pointer operand on the right hand side of the pointer assignment statement.
   */
  explicit PointerAssignedPointee(Pointer *pointer) noexcept
    : PointerAssignment { PointerAssignmentKind::AssignedPointee, pointer }
  { }
};

/**
 * Represent a pointer assignment statement of the form `*p = q`.
 */
class PointeeAssignedPointer : public PointerAssignment {
public:
  static bool classof(const PointerAssignment *obj) noexcept {
    return obj->kind() == PointerAssignmentKind::PointeeAssigned;
  }

  /**
   * Construct a new PointeeAssignedPointer object.
   *
   * @param pointer the pointer operand on the right hand side of the pointer assignment statement.
   */
  explicit PointeeAssignedPointer(Pointer *pointer) noexcept
    : PointerAssignment { PointerAssignmentKind::PointeeAssigned, pointer }
  { }
};

/**
 * A set of GetPointeeSet.
 */
class PointeeSet {
public:
  class const_iterator;

  /**
   * Iterator of PointeeSet.
   */
  class iterator {
  public:
    /**
     * Type of the inner iterator.
     */
    using inner_iterator = typename std::unordered_set<Pointee *>::iterator;

    /**
     * Construct a new iterator object from the given inner iterator.
     *
     * @param inner the inner iterator.
     */
    explicit iterator(inner_iterator inner) noexcept
      : _inner(inner)
    { }

    Pointee* operator*() const noexcept {
      return *_inner;
    }

    iterator& operator++() noexcept {
      ++_inner;
      return *this;
    }

    iterator operator++(int) & noexcept { // NOLINT(cert-dcl21-cpp)
      auto old = *this;
      ++_inner;
      return old;
    }

    bool operator==(const iterator &rhs) const noexcept {
      return _inner == rhs._inner;
    }

    bool operator!=(const iterator &rhs) const noexcept {
      return _inner != rhs._inner;
    }

    friend class PointeeSet::const_iterator;

  private:
    inner_iterator _inner;
  };

  /**
   * Constant iterator of PointeeSet.
   */
  class const_iterator {
  public:
    /**
     * Type of the inner iterator.
     */
    using inner_iterator = typename std::unordered_set<Pointee *>::const_iterator;

    /**
     * Construct a new const_iterator object from the given inner iterator.
     *
     * @param inner the inner iterator.
     */
    explicit const_iterator(inner_iterator inner) noexcept
      : _inner(inner)
    { }

    const_iterator(iterator iter) noexcept // NOLINT(google-explicit-constructor)
      : _inner(iter._inner)
    { }

    const Pointee* operator*() const noexcept {
      return *_inner;
    }

    const_iterator& operator++() noexcept {
      ++_inner;
      return *this;
    }

    const_iterator operator++(int) & noexcept { // NOLINT(cert-dcl21-cpp)
      auto old = *this;
      ++_inner;
      return old;
    }

    bool operator==(const const_iterator &rhs) const noexcept {
      return _inner == rhs._inner;
    }

    bool operator!=(const const_iterator &rhs) const noexcept {
      return _inner != rhs._inner;
    }

  private:
    inner_iterator _inner;
  };

  /**
   * Get the number of elements contained in the PointeeSet.
   *
   * @return the number of elements contained in the PointeeSet.
   */
  size_t size() const noexcept {
    return _pointees.size();
  }

  iterator begin() noexcept {
    return iterator { _pointees.begin() };
  }

  const_iterator begin() const noexcept {
    return cbegin();
  }

  iterator end() noexcept  {
    return iterator { _pointees.end() };
  }

  const_iterator end() const noexcept  {
    return cend();
  }

  const_iterator cbegin() noexcept {
    return const_iterator { _pointees.cbegin() };
  }

  const_iterator cbegin() const noexcept {
    return const_iterator { _pointees.cbegin() };
  }

  const_iterator cend() noexcept {
    return const_iterator { _pointees.cend() };
  }

  const_iterator cend() const noexcept {
    return const_iterator { _pointees.cend() };
  }

  /**
   * Insert the given pointee into this set.
   *
   * @param pointee the pointee.
   * @return whether the insertion takes place.
   */
  bool insert(Pointee *pointee) noexcept {
    return _pointees.insert(pointee).second;
  }

  /**
   * Get the iterator to the specified element.
   *
   * @param pointee the element to find.
   * @return the iterator to the specified element. If no such element are contained in this set, returns `end()`.
   */
  iterator find(Pointee *pointee) noexcept {
    auto inner = _pointees.find(pointee);
    return iterator { inner };
  }

  /**
   * Get the iterator to the specified element.
   *
   * @param pointee the element to find.
   * @return the iterator to the specified element. If no such element are contained in this set, returns `end()`.
   */
  const_iterator find(const Pointee *pointee) const noexcept {
    auto inner = _pointees.find(const_cast<Pointee *>(pointee));
    return const_iterator { inner };
  }

  /**
   * Return 1 if `pointee` is in this set, otherwise return 0.
   *
   * @return 1 if `pointee` is in this set, otherwise return 0.
   */
  size_t count(const Pointee *pointee) const noexcept {
    auto it = find(pointee);
    if (it == end()) {
      return 0;
    }
    return 1;
  }

  /**
   * Determine whether the specified set is a subset of this set.
   *
   * @param another another pointee set.
   * @return whether the specified set is a subset of this set.
   */
  bool isSubset(const PointeeSet &another) const noexcept {
    return std::all_of(another.begin(), another.end(), [this](const Pointee *anotherElement) {
      return count(anotherElement) == 1;
    });
  }

  /**
   * Determine whether this set is a subset of the specified set.
   *
   * @param another another pointee set.
   * @return whether this set is a subset of the specified set.
   */
  bool isSubsetOf(const PointeeSet &another) const noexcept {
    return another.isSubset(*this);
  }

  /**
   * Merge all elements from the specified set into this set.
   *
   * @param source the source pointee set.
   * @return whether at least one new element is added into this set.
   */
  bool MergeFrom(const PointeeSet &source) noexcept {
    auto newElement = false;
    for (auto sourceElement : source) {
      if (insert(const_cast<Pointee *>(sourceElement))) {
        newElement = true;
      }
    }
    return newElement;
  }

  /**
   * Merge all elements from this set into the specified set.
   *
   * @param target the target set.
   * @return whether at least one new element is added to the specified set.
   */
  bool MergeTo(PointeeSet &target) const noexcept {
    return target.MergeFrom(*this);
  }

  bool operator==(const PointeeSet &rhs) const noexcept {
    return _pointees == rhs._pointees;
  }

  bool operator!=(const PointeeSet &rhs) const noexcept {
    return _pointees != rhs._pointees;
  }

  PointeeSet& operator+=(const PointeeSet &rhs) noexcept {
    MergeFrom(rhs);
    return *this;
  }

private:
  std::unordered_set<Pointee *> _pointees;
};

/**
 * Represent a possible pointee of some pointer.
 */
class Pointee {
public:
  /**
   * Construct a new Pointee object.
   *
   * @param node the location of the pointee in the value tree.
   */
  explicit Pointee(ValueTreeNode &node) noexcept
    : _node(node)
  { }

  NON_COPIABLE_NON_MOVABLE(Pointee)

  /**
   * Get the location of this pointee in the value tree.
   *
   * @return the location of this pointee in the value tree.
   */
  ValueTreeNode* node() noexcept {
    return &_node;
  }

  /**
   * Get the location of this pointee in the value tree.
   *
   * @return the location of this pointee in the value tree.
   */
  const ValueTreeNode* node() const noexcept {
    return &_node;
  }

  /**
   * Determine whether this pointee is a pointer.
   *
   * @return whether this pointee is a pointer.
   */
  bool isPointer() const noexcept;

  /**
   * Determine whether this pointee is defined outside of the current module.
   *
   * @return whether this pointee is defined outside of the current module.
   */
  bool isExternal() const noexcept;

private:
  ValueTreeNode &_node;
};

/**
 * Represent a pointer.
 */
class Pointer : public Pointee {
public:
  static bool classof(const Pointee *obj) noexcept {
    return obj->isPointer();
  }

  /**
   * Construct a new Pointer object.
   *
   * @param node the location of the pointer in the value tree.
   */
  explicit Pointer(ValueTreeNode &node) noexcept
    : Pointee { node },
      _assignedElementPtr(),
      _assignedPointee(),
      _pointeeAssigned(),
      _pointees()
  { }

  NON_COPIABLE_NON_MOVABLE(Pointer)

  /**
   * Specify that this pointer is assigned to the specified pointer somewhere in the program.
   *
   * @param pointer the pointer on the right hand side of the pointer assignment.
   */
  void AssignedPointer(Pointer *pointer) noexcept {
    AssignedElementPtr(pointer, { PointerIndex { 0 } });
  }

  /**
   * Specify that this pointer is assigned to the address of some element in the pointee of the specified pointer, with
   * the specified pointer index sequence.
   *
   * @param pointer the pointer on the right hand side of the pointer assignment.
   * @param indexSequence the pointer index sequence.
   */
  void AssignedElementPtr(Pointer *pointer, std::vector<PointerIndex> indexSequence) noexcept {
    assert(pointer && "pointer cannot be null");
    _assignedElementPtr.emplace(pointer, std::move(indexSequence));
  }

  /**
   * Specify that this pointer is assigned to the pointee of the specified pointer.
   *
   * @param pointer the pointer on the right hand side of the pointer assignment.
   */
  void AssignedPointee(Pointer *pointer) noexcept {
    assert(pointer && "pointer cannot be null");
    _assignedPointee.emplace(pointer);
  }

  /**
   * Specify that the pointee of this pointer is assigned to the specified pointer.
   *
   * @param pointer the pointer on the right hand side of the pointer assignment.
   */
  void PointeeAssigned(Pointer *pointer) noexcept {
    assert(pointer && "pointer cannot be null");
    _pointeeAssigned.emplace(pointer);
  }

  /**
   * Get the pointee set of this pointer.
   *
   * @return the pointee set of this pointer.
   */
  PointeeSet& GetPointeeSet() noexcept {
    return _pointees;
  }

  /**
   * Get the pointee set of this pointer.
   *
   * @return the pointee set of this pointer.
   */
  const PointeeSet& GetPointeeSet() const noexcept {
    return _pointees;
  }

private:
  std::unordered_set<PointerAssignedElementPtr, details::PolymorphicHasher<PointerAssignedElementPtr>> _assignedElementPtr;
  std::unordered_set<PointerAssignedPointee, details::PolymorphicHasher<PointerAssignedPointee>> _assignedPointee;
  std::unordered_set<PointeeAssignedPointer, details::PolymorphicHasher<PointeeAssignedPointer>> _pointeeAssigned;
  PointeeSet _pointees;
};

enum class ValueKind {
  Normal,
  StackMemory,
  GlobalMemory,
};

/**
 * A node in the value tree.
 */
class ValueTreeNode {
public:
  /**
   * Construct a new ValueTreeNode object that represents the specified value.
   *
   * @param value the `llvm::Value` of the new node.
   */
  explicit ValueTreeNode(const llvm::Value *value) noexcept
    : _type(value->getType()),
      _value(value),
      _kind(ValueKind::Normal),
      _parent(nullptr),
      _offset(0),
      _children()
  {
    assert(value && "value cannot be null");
    Initialize();
  }

  explicit ValueTreeNode(const llvm::AllocaInst *stackMemoryAllocator) noexcept
    : _type(stackMemoryAllocator->getAllocatedType()),
      _value(stackMemoryAllocator),
      _kind(ValueKind::StackMemory),
      _parent(nullptr),
      _offset(0),
      _children()
  {
    assert(stackMemoryAllocator && "stackMemoryAllocator cannot be null");
    Initialize();
  }

  explicit ValueTreeNode(const llvm::GlobalVariable *globalVariable) noexcept
    : _type(globalVariable->getValueType()),
      _value(globalVariable),
      _kind(ValueKind::GlobalMemory),
      _parent(nullptr),
      _offset(0),
      _children()
  {
    assert(globalVariable && "globalVariable cannot be null");
    Initialize();
  }

  /**
   * Construct a new ValueTreeNode object that represents the sub-object of the specified parent value.
   *
   * @param type type of this sub-object.
   * @param parent ValueTreeNode that represents the parent value.
   * @param offset the offset of this sub-object within the parent value.
   */
  explicit ValueTreeNode(const llvm::Type *type, ValueTreeNode *parent, size_t offset) noexcept
    : _type(type),
      _value(nullptr),
      _kind(parent->_kind),
      _parent(parent),
      _offset(offset),
      _children()
  {
    assert(type && "type cannot be null");
    assert(parent && "parent cannot be null");
    Initialize();
  }

  NON_COPIABLE_NON_MOVABLE(ValueTreeNode)

  /**
   * Get the type of this value.
   *
   * @return the type of this value.
   */
  const llvm::Type *type() const noexcept {
    return _type;
  }

  /**
   * Get the value represented by this node as `llvm::Value` object.
   *
   * @return the value represented by this node as `llvm::Value` object. If this node represents a sub-object of some
   * parent value, return nullptr.
   */
  const llvm::Value *value() const noexcept {
    return _value;
  }

  /**
   * Get the kind of this value.
   *
   * @return the kind of this value.
   */
  ValueKind kind() const noexcept {
    return _kind;
  }

  /**
   * Determine whether this value is within a region of stack allocated memory.
   *
   * @return whether this value is within a region of stack allocated memory.
   */
  bool isStackMemory() const noexcept {
    return _kind == ValueKind::StackMemory;
  }

  /**
   * Determine whether this value is within a region of global memory.
   *
   * @return whether this value is within a region of global memory.
   */
  bool isGlobalMemory() const noexcept {
    return _kind == ValueKind::GlobalMemory;
  }

  /**
   * Get the parent node of this node.
   *
   * @return the parent node of this node. If this node does not represent a sub-object of some parent value, return
   * nullptr.
   */
  ValueTreeNode* parent() const noexcept {
    return _parent;
  }

  /**
   * Get the offset of the sub-object within the parent object.
   *
   * @return the offset of the sub-object within the parent object. The return value is always 0 if this node does not
   * represent a sub-object.
   */
  size_t offset() const noexcept {
    return _offset;
  }

  /**
   * Get the Pointee object connected to this node.
   *
   * @return the Pointee object connected to this node.
   */
  Pointee* pointee() noexcept {
    return _pointee.get();
  }

  /**
   * Get the Pointee object connected to this node.
   *
   * @return the Pointee object connected to this node.
   */
  const Pointee* pointee() const noexcept {
    return _pointee.get();
  }

  /**
   * Determine whether this node represent a root value, i.e. this value is not a sub-object.
   *
   * @return whether this node represent a root value.
   */
  bool isRoot() const noexcept {
    return _parent == nullptr;
  }

  /**
   * Determine whether the value represented by this node is in global scope.
   *
   * @return whether the value represented by this node is in global scope.
   */
  bool isGlobal() const noexcept {
    if (_parent) {
      return _parent->isGlobal();
    }
    return llvm::isa<llvm::GlobalObject>(_value);
  }

  /**
   * Determine whether the value represented by this node is defined outside of the current module.
   *
   * @return whether the value represented by this node is defined outside of the current module.
   */
  bool isExternal() const noexcept {
    if (_parent) {
      return _parent->isExternal();
    }

    if (!isGlobal()) {
      return false;
    }

    auto globalObject = llvm::cast<llvm::GlobalObject>(_value);
    return llvm::GlobalValue::isAvailableExternallyLinkage(globalObject->getLinkage());
  }

  /**
   * Determine whether the value represented by this node is a pointer.
   *
   * @return whether the value represented by this node is a pointer.
   */
  bool isPointer() const noexcept {
    return _type->isPointerTy();
  }

  /**
   * Get the Pointer object connected to this node.
   *
   * If this node does not represent a pointer value, this function triggers an assertion failure.
   *
   * @return the pointer object connected to this node.
   */
  Pointer* pointer() noexcept {
    return llvm::cast<Pointer>(pointee());
  }

  /**
   * Get the Pointer object connected to this node.
   *
   * If this node does not represent a pointer value, this function triggers an assertion failure.
   *
   * @return the pointer object connected to this node.
   */
  const Pointer* pointer() const noexcept {
    return llvm::cast<Pointer>(pointee());
  }

  /**
   * Get the `alloca` instruction that allocates this stack memory region.
   *
   * This function triggers an assertion failure if the value represented by this ValueTreeNode is not a stack memory.
   *
   * @return the `alloca` instruction that allocates this stack memory region.
   */
  const llvm::AllocaInst* GetStackMemoryAllocator() const noexcept {
    if (!isRoot()) {
      return _parent->GetStackMemoryAllocator();
    }
    return llvm::cast<llvm::AllocaInst>(_value);
  }

  /**
   * Get the global variable that refers to this global memory region.
   *
   * This function triggers an assertion failure if the value represented by this ValueTreeNode is not a global memory.
   *
   * @return the global variable that refers to this global memory region.
   */
  const llvm::GlobalVariable* GetGlobalVariable() const noexcept {
    if (!isRoot()) {
      return _parent->GetGlobalVariable();
    }
    return llvm::cast<llvm::GlobalVariable>(_value);
  }

  /**
   * Determine whether this node has any child nodes.
   *
   * @return whether this node has any child nodes.
   */
  bool hasChildren() const noexcept {
    return !_children.empty();
  }

  /**
   * Get the number of child nodes under this node.
   *
   * @return the number of child nodes under this node.
   */
  size_t GetNumChildren() const noexcept {
    return _children.size();
  }

  /**
   * Get the child node at the specified index.
   *
   * If the index is out of range, this function triggers an assertion failure.
   *
   * @param index the index.
   * @return the child node at the specified index.
   */
  ValueTreeNode* GetChild(size_t index) noexcept {
    assert(index >= 0 && index < _children.size() && "index is out of range");
    return _children[index].get();
  }

  /**
   * Get the child node at the specified index.
   *
   * If the index is out of range, this function triggers an assertion failure.
   *
   * @param index the index.
   * @return the child node at the specified index.
   */
  const ValueTreeNode* GetChild(size_t index) const noexcept {
    assert(index >= 0 && index < _children.size() && "index is out of range");
    return _children[index].get();
  }

private:
  const llvm::Type *_type;
  const llvm::Value *_value;
  ValueKind _kind;
  ValueTreeNode *_parent;
  size_t _offset;
  std::vector<std::unique_ptr<ValueTreeNode>> _children;
  std::unique_ptr<Pointee> _pointee;

  void Initialize() noexcept {
    InitializePointee();
    InitializeChildren();
  }

  void InitializeChildren() noexcept;

  void InitializePointee() noexcept {
    if (_type->isPointerTy()) {
      _pointee = std::make_unique<Pointer>(*this);
    } else {
      _pointee = std::make_unique<Pointee>(*this);
    }
  }
};

/**
 * The value tree that represents the value hierarchy of a program.
 */
class ValueTree {
public:
  /**
   * Construct a new ValueTree object.
   *
   * This constructor builds all possible value trees for each rooted value in the specified module.
   *
   * @param module the LLVM module.
   */
  explicit ValueTree(const llvm::Module &module) noexcept;

  /**
   * Get the value tree node corresponding to the specified rooted value.
   *
   * @param value the rooted value.
   * @return the value tree node corresponding to the specified rooted value. If the specified value is not a valid root
   * of a value tree, return nullptr.
   */
  ValueTreeNode* GetNode(const llvm::Value *value) noexcept {
    auto ptr = details::find_in(_roots, value);
    if (ptr) {
      return ptr->get();
    }
    return nullptr;
  }

  /**
   * Get the value tree node corresponding to the specified rooted value.
   *
   * @param value the rooted value.
   * @return the value tree node corresponding to the specified rooted value. If the specified value is not a valid root
   * of a value tree, return nullptr.
   */
  const ValueTreeNode* GetNode(const llvm::Value *value) const noexcept {
    return const_cast<ValueTree *>(this)->GetNode(value);
  }

  /**
   * Get the ValueTreeNode corresponding to the stack memory allocated by the specified `alloca` instruction.
   *
   * @param inst the `alloca` instruction that allocates the stack memory.
   * @return the ValueTreeNode corresponding to the stack memory.
   */
  ValueTreeNode* GetAllocaNode(const llvm::AllocaInst *inst) noexcept {
    auto ptr = details::find_in(_allocaRoots, inst);
    if (ptr) {
      return ptr->get();
    }
    return nullptr;
  }

  /**
   * Get the ValueTreeNode corresponding to the stack memory allocated by the specified `alloca` instruction.
   *
   * @param inst the `alloca` instruction that allocates the stack memory.
   * @return the ValueTreeNode corresponding to the stack memory.
   */
  const ValueTreeNode* GetAllocaNode(const llvm::AllocaInst *inst) const noexcept {
    return const_cast<ValueTree *>(this)->GetAllocaNode(inst);
  }

  /**
   * Get the ValueTreeNode corresponding to the global memory referred to by the specified global variable pointer.
   *
   * @param variable the global variable pointer.
   * @return the ValueTreeNode corresponding to the global memory.
   */
  ValueTreeNode* GetGlobalNode(const llvm::GlobalVariable *variable) noexcept {
    auto ptr = details::find_in(_globalRoots, variable);
    if (ptr) {
      return ptr->get();
    }
    return nullptr;
  }

  /**
   * Get the ValueTreeNode corresponding to the global memory referred to by the specified global variable pointer.
   *
   * @param variable the global variable pointer.
   * @return the ValueTreeNode corresponding to the global memory.
   */
  const ValueTreeNode* GetGlobalNode(const llvm::GlobalVariable *variable) const noexcept {
    return const_cast<ValueTree *>(this)->GetGlobalNode(variable);
  }

private:
  const llvm::Module &_module;
  std::unordered_map<const llvm::Value *, std::unique_ptr<ValueTreeNode>> _roots;
  std::unordered_map<const llvm::AllocaInst *, std::unique_ptr<ValueTreeNode>> _allocaRoots;
  std::unordered_map<const llvm::GlobalVariable *, std::unique_ptr<ValueTreeNode>> _globalRoots;
};

/**
 * Implementation of Anderson points-to analysis algorithm as a LLVM module pass.
 */
class AndersonPointsToAnalysis : public llvm::ModulePass {
public:
  static char ID;

  /**
   * Construct a new AndersonPointsToAnalysis object.
   */
  explicit AndersonPointsToAnalysis() noexcept
    : llvm::ModulePass { ID },
      _valueTree(nullptr)
  { }

  NON_COPIABLE_NON_MOVABLE(AndersonPointsToAnalysis)

  bool runOnModule(llvm::Module &module) final;

  /**
   * Get the value tree which contains analysis result.
   *
   * @return the value tree which contains analysis result.
   */
  ValueTree* GetValueTree() noexcept {
    assert(_valueTree && "The analysis has not been run");
    return _valueTree.get();
  }

  /**
   * Get the value tree which contains analysis result.
   *
   * @return the value tree which contains analysis result.
   */
  const ValueTree* GetValueTree() const noexcept {
    assert(_valueTree && "The analysis has not been run");
    return _valueTree.get();
  }

private:
  std::unique_ptr<ValueTree> _valueTree;
};

bool Pointee::isPointer() const noexcept {
  return _node.isPointer();
}

bool Pointee::isExternal() const noexcept {
  return _node.isExternal();
}

} // namespace anderson

} // namespace llvm

#endif // LLVM_ANDERSON_POINTS_TO_ANALYSIS_H

#pragma clang diagnostic pop