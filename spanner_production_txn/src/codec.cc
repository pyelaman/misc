#include "spanner_prod/codec.h"

#include <cstring>
#include <limits>
#include <type_traits>

namespace spanner_prod {
namespace {

class Encoder {
 public:
  template <typename T>
  void Number(T value) {
    static_assert(std::is_integral_v<T>);
    using U = std::make_unsigned_t<T>;
    U bits = static_cast<U>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      bytes_.push_back(static_cast<char>((bits >> (i * 8U)) & 0xffU));
    }
  }

  void String(std::string_view value) {
    Number<std::uint32_t>(static_cast<std::uint32_t>(value.size()));
    bytes_.append(value);
  }

  std::string Finish() { return std::move(bytes_); }

 private:
  std::string bytes_;
};

class Decoder {
 public:
  explicit Decoder(std::string_view bytes) : bytes_(bytes) {}

  template <typename T>
  bool Number(T* value) {
    static_assert(std::is_integral_v<T>);
    if (offset_ + sizeof(T) > bytes_.size()) {
      return false;
    }
    using U = std::make_unsigned_t<T>;
    U bits = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      bits |= static_cast<U>(static_cast<unsigned char>(bytes_[offset_ + i]))
              << (i * 8U);
    }
    *value = static_cast<T>(bits);
    offset_ += sizeof(T);
    return true;
  }

  bool String(std::string* value) {
    std::uint32_t length = 0;
    if (!Number(&length) || offset_ + length > bytes_.size()) {
      return false;
    }
    value->assign(bytes_.substr(offset_, length));
    offset_ += length;
    return true;
  }

  [[nodiscard]] bool Done() const { return offset_ == bytes_.size(); }

 private:
  std::string_view bytes_;
  std::size_t offset_{0};
};

void EncodeCell(Encoder& encoder, const Cell& cell) {
  encoder.String(cell.table);
  encoder.String(cell.row);
  encoder.String(cell.column);
}

bool DecodeCell(Decoder& decoder, Cell* cell) {
  return decoder.String(&cell->table) && decoder.String(&cell->row) &&
         decoder.String(&cell->column);
}

void EncodeTransaction(Encoder& encoder,
                       const TransactionDescriptor& transaction) {
  encoder.Number(transaction.id);
  encoder.Number(transaction.priority);
  encoder.Number(transaction.deadline);
  encoder.Number(transaction.coordinator_split);
  encoder.String(transaction.tag);
}

bool DecodeTransaction(Decoder& decoder,
                       TransactionDescriptor* transaction) {
  return decoder.Number(&transaction->id) &&
         decoder.Number(&transaction->priority) &&
         decoder.Number(&transaction->deadline) &&
         decoder.Number(&transaction->coordinator_split) &&
         decoder.String(&transaction->tag);
}

void EncodeLock(Encoder& encoder, const LockRequest& lock) {
  EncodeCell(encoder, lock.cell);
  encoder.Number(static_cast<std::uint8_t>(lock.mode));
}

bool DecodeLock(Decoder& decoder, LockRequest* lock) {
  std::uint8_t mode = 0;
  if (!DecodeCell(decoder, &lock->cell) || !decoder.Number(&mode) ||
      mode > static_cast<std::uint8_t>(LockMode::kWriterSharedTimestamp)) {
    return false;
  }
  lock->mode = static_cast<LockMode>(mode);
  return true;
}

void EncodeMutation(Encoder& encoder, const Mutation& mutation) {
  EncodeCell(encoder, mutation.cell);
  encoder.Number<std::uint8_t>(mutation.value.has_value() ? 1 : 0);
  if (mutation.value.has_value()) {
    encoder.String(*mutation.value);
  }
}

bool DecodeMutation(Decoder& decoder, Mutation* mutation) {
  std::uint8_t present = 0;
  if (!DecodeCell(decoder, &mutation->cell) || !decoder.Number(&present) ||
      present > 1) {
    return false;
  }
  if (present != 0) {
    std::string value;
    if (!decoder.String(&value)) {
      return false;
    }
    mutation->value = std::move(value);
  } else {
    mutation->value = std::nullopt;
  }
  return true;
}

template <typename T, typename Encode>
void EncodeVector(Encoder& encoder, const std::vector<T>& values,
                  Encode encode) {
  encoder.Number<std::uint32_t>(static_cast<std::uint32_t>(values.size()));
  for (const auto& value : values) {
    encode(encoder, value);
  }
}

template <typename T, typename Decode>
bool DecodeVector(Decoder& decoder, std::vector<T>* values, Decode decode) {
  std::uint32_t count = 0;
  if (!decoder.Number(&count) || count > 1'000'000) {
    return false;
  }
  values->clear();
  values->reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    T value;
    if (!decode(decoder, &value)) {
      return false;
    }
    values->push_back(std::move(value));
  }
  return true;
}

}  // namespace

std::string EncodeLogEntry(const LogEntry& entry) {
  Encoder encoder;
  encoder.Number<std::uint32_t>(0x53504e52U);  // SPNR
  encoder.Number<std::uint16_t>(1);
  encoder.Number(entry.term);
  encoder.Number(entry.index);
  encoder.Number(entry.operation_id);
  encoder.Number<std::uint8_t>(
      static_cast<std::uint8_t>(entry.command.index()));
  std::visit(
      [&encoder](const auto& command) {
        using Command = std::decay_t<decltype(command)>;
        if constexpr (std::is_same_v<Command, PrepareCommand>) {
          EncodeTransaction(encoder, command.prepared.transaction);
          encoder.Number(command.prepared.prepare_timestamp);
          EncodeVector(encoder, command.prepared.locks, EncodeLock);
          EncodeVector(encoder, command.prepared.mutations, EncodeMutation);
        } else if constexpr (std::is_same_v<Command, CommitCommand>) {
          encoder.Number(command.transaction_id);
          encoder.Number(command.commit_timestamp);
        } else if constexpr (std::is_same_v<Command, AbortCommand>) {
          encoder.Number(command.transaction_id);
          encoder.String(command.reason);
        } else if constexpr (std::is_same_v<Command,
                                            RecordDecisionCommand>) {
          encoder.Number(command.decision.transaction_id);
          encoder.Number<std::uint8_t>(
              static_cast<std::uint8_t>(command.decision.kind));
          encoder.Number(command.decision.commit_timestamp);
          EncodeVector<SplitId>(
              encoder, command.decision.participants,
              [](Encoder& target, SplitId split) { target.Number(split); });
          encoder.String(command.decision.reason);
        } else if constexpr (std::is_same_v<Command,
                                            AdvanceSafeTimeCommand>) {
          encoder.Number(command.timestamp);
        }
      },
      entry.command);
  return encoder.Finish();
}

StatusOr<LogEntry> DecodeLogEntry(std::string_view bytes) {
  Decoder decoder(bytes);
  std::uint32_t magic = 0;
  std::uint16_t version = 0;
  LogEntry entry;
  std::uint8_t kind = 0;
  if (!decoder.Number(&magic) || magic != 0x53504e52U ||
      !decoder.Number(&version) || version != 1 ||
      !decoder.Number(&entry.term) || !decoder.Number(&entry.index) ||
      !decoder.Number(&entry.operation_id) || !decoder.Number(&kind)) {
    return Status::Error(StatusCode::kCorruption,
                         "invalid replicated-command header");
  }

  switch (kind) {
    case 0: {
      PrepareCommand command;
      if (!DecodeTransaction(decoder, &command.prepared.transaction) ||
          !decoder.Number(&command.prepared.prepare_timestamp) ||
          !DecodeVector(decoder, &command.prepared.locks, DecodeLock) ||
          !DecodeVector(decoder, &command.prepared.mutations,
                        DecodeMutation)) {
        return Status::Error(StatusCode::kCorruption,
                             "invalid prepare command");
      }
      entry.command = std::move(command);
      break;
    }
    case 1: {
      CommitCommand command;
      if (!decoder.Number(&command.transaction_id) ||
          !decoder.Number(&command.commit_timestamp)) {
        return Status::Error(StatusCode::kCorruption,
                             "invalid commit command");
      }
      entry.command = command;
      break;
    }
    case 2: {
      AbortCommand command;
      if (!decoder.Number(&command.transaction_id) ||
          !decoder.String(&command.reason)) {
        return Status::Error(StatusCode::kCorruption,
                             "invalid abort command");
      }
      entry.command = std::move(command);
      break;
    }
    case 3: {
      RecordDecisionCommand command;
      std::uint8_t decision = 0;
      if (!decoder.Number(&command.decision.transaction_id) ||
          !decoder.Number(&decision) || decision > 1 ||
          !decoder.Number(&command.decision.commit_timestamp) ||
          !DecodeVector<SplitId>(
              decoder, &command.decision.participants,
              [](Decoder& source, SplitId* split) {
                return source.Number(split);
              }) ||
          !decoder.String(&command.decision.reason)) {
        return Status::Error(StatusCode::kCorruption,
                             "invalid decision command");
      }
      command.decision.kind = static_cast<DecisionKind>(decision);
      entry.command = std::move(command);
      break;
    }
    case 4: {
      AdvanceSafeTimeCommand command;
      if (!decoder.Number(&command.timestamp)) {
        return Status::Error(StatusCode::kCorruption,
                             "invalid safe-time command");
      }
      entry.command = command;
      break;
    }
    default:
      return Status::Error(StatusCode::kCorruption,
                           "unknown replicated command kind");
  }
  if (!decoder.Done()) {
    return Status::Error(StatusCode::kCorruption,
                         "trailing bytes in replicated command");
  }
  return entry;
}

std::uint32_t Checksum(std::string_view bytes) {
  std::uint32_t hash = 2166136261U;
  for (const unsigned char byte : bytes) {
    hash ^= byte;
    hash *= 16777619U;
  }
  return hash;
}

}  // namespace spanner_prod

