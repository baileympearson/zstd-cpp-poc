#include <iostream>
#include <string>
#include <vector>
#include <optional>
#include "zstd.h"
#include <napi.h>

using namespace Napi;

typedef std::vector<uint8_t> byte_vec;

struct FrameHeader
{
	struct Deleter
	{
		void operator()(ZSTD_frameHeader *header) noexcept
		{
			free(header);
		}
	};

	unsigned long long frameContentSize;
	unsigned long long windowSize;
	ZSTD_frameType_e frameType;
	unsigned headerSize;
	unsigned dictID;
	unsigned checksumFlag;
	unsigned _reserved1;
	unsigned _reserved2;

	FrameHeader(Uint8Array array);

private:
	FrameHeader(std::unique_ptr<ZSTD_frameHeader, FrameHeader::Deleter> ptr);

	static std::unique_ptr<ZSTD_frameHeader, FrameHeader::Deleter> parseFrameHeader(Uint8Array buffer);
};

FrameHeader::FrameHeader(std::unique_ptr<ZSTD_frameHeader, FrameHeader::Deleter> ptr) : frameContentSize(ptr->frameContentSize),
																						windowSize(ptr->windowSize),
																						frameType(ptr->frameType),
																						headerSize(ptr->headerSize),
																						dictID(ptr->dictID),
																						checksumFlag(ptr->checksumFlag)

{};

std::unique_ptr<ZSTD_frameHeader, FrameHeader::Deleter> FrameHeader::parseFrameHeader(Uint8Array buffer)
{
	std::unique_ptr<ZSTD_frameHeader, FrameHeader::Deleter> ptr((ZSTD_frameHeader *)malloc(sizeof(ZSTD_frameHeader)));
	auto result = ZSTD_getFrameHeader(ptr.get(), buffer.Data(), buffer.ByteLength());

	if (ZSTD_isError(result))
	{
		std::string error_message = std::string("Error parsing frame header: ") + ZSTD_getErrorName(result);
		throw Error::New(buffer.Env(), error_message);
	}

	return ptr;
}

FrameHeader::FrameHeader(Uint8Array array) : FrameHeader(parseFrameHeader(array)) {};

byte_vec decompress(byte_vec &compressed, size_t buffer_size)
{
	byte_vec decompressed(buffer_size);

	size_t _result = ZSTD_decompress(decompressed.data(), decompressed.size(), compressed.data(), compressed.size());

	if (ZSTD_isError(_result))
	{
		std::cout << ZSTD_getErrorName(_result) << std::endl;
		std::exit(1);
	}

	decompressed.resize(_result);

	return decompressed;
}

byte_vec decompress(byte_vec &compressed)
{
	return decompress(compressed, compressed.size() * 50);
}

byte_vec compress(byte_vec &to_compress, size_t compression_level)
{
	auto output_buffer_size = ZSTD_compressBound(to_compress.size());
	byte_vec output(output_buffer_size);

	size_t _result = ZSTD_compress(output.data(), output.size(), to_compress.data(), to_compress.size(), compression_level);

	if (ZSTD_isError(_result))
	{
		std::cout << "error: " << ZSTD_getErrorName(_result) << std::endl;
		std::exit(1);
	}

	output.resize(_result);

	return output;
}

byte_vec compress(std::string &to_compress, size_t compression_level)
{
	byte_vec data(to_compress.begin(), to_compress.end());
	return compress(data, compression_level);
}

Uint8Array Uint8ArrayFromValue(Napi::Value v, std::string argument_name)
{
	if (!v.IsTypedArray() || v.As<TypedArray>().TypedArrayType() != napi_uint8_array)
	{
		std::string error_message = "Parameter `" + argument_name + "` must be a Uint8Array.";
		throw TypeError::New(v.Env(), error_message);
	}

	return v.As<Uint8Array>();
}

template <typename T>
void copy_buffer_data(T *source, T *dest, size_t count)
{
	for (size_t i = 0; i < count; ++i)
	{
		dest[i] = source[i];
	}
}

class CompressWorker : public Napi::AsyncWorker
{
public:
	CompressWorker(const Napi::Env& env, const byte_vec data, size_t compression_level)
		: Napi::AsyncWorker{env, "CompressWorker"},
		  m_deferred{env},
		  data(data),
		  compression_level(compression_level),
		  result{} {}

	/**
	 * GetPromise associated with _deferred for return to JS
	 */
	Napi::Promise GetPromise() { return m_deferred.Promise(); }

protected:
	/**
	 * Simulate heavy math work
	 */
	void Execute()
	{
		result = compress(data, compression_level);
	}

	/**
	 * Resolve the promise with the result
	 */
	void OnOK()
	{
		Uint8Array output = Uint8Array::New(Env(), result.size());

		copy_buffer_data(result.data(), output.Data(), result.size());

		m_deferred.Resolve(output);
	}

	/**
	 * Reject the promise with errors
	 */
	void OnError(const Napi::Error &err)
	{
		m_deferred.Reject(err.Value());
	}

private:
	Napi::Promise::Deferred m_deferred;
	byte_vec data;
	size_t compression_level;
	byte_vec result;
};

class DecompressWorker : public Napi::AsyncWorker
{
public:
	DecompressWorker(const Napi::Env& env, const byte_vec data, size_t buffer_size)
		: Napi::AsyncWorker{env, "DecompressWorker"},
		  m_deferred{env},
		  data(data),
		  buffer_size(buffer_size),
		  result(buffer_size) {}

	/**
	 * GetPromise associated with _deferred for return to JS
	 */
	Napi::Promise GetPromise() { return m_deferred.Promise(); }

protected:
	/**
	 * Simulate heavy math work
	 */
	void Execute()
	{
		result = decompress(data, buffer_size);
	}

	/**
	 * Resolve the promise with the result
	 */
	void OnOK()
	{
		Uint8Array output = Uint8Array::New(Env(), result.size());

		copy_buffer_data(result.data(), output.Data(), result.size());

		m_deferred.Resolve(output);
	}

	/**
	 * Reject the promise with errors
	 */
	void OnError(const Napi::Error &err)
	{
		m_deferred.Reject(err.Value());
	}

private:
	Napi::Promise::Deferred m_deferred;
	byte_vec data;
	size_t buffer_size;
	byte_vec result;
};

Napi::Promise Compress(const Napi::CallbackInfo &info)
{
	auto number_of_args = info.Length();
	size_t compression_level;
	Napi::Uint8Array to_compress;

	if (number_of_args == 0)
	{
		std::string error_message = "compress(uint8array) or compress(uint8array, compression level)";
		throw TypeError::New(info.Env(), error_message);
	}
	else if (number_of_args == 1)
	{
		to_compress = Uint8ArrayFromValue(info[0], "buffer");
		compression_level = 3;
	}
	else if (number_of_args == 2)
	{
		to_compress = Uint8ArrayFromValue(info[0], "buffer");
		if (!info[1].IsNumber())
		{
			throw TypeError::New(info.Env(), std::string("if provided, compression_level must be a number."));
		}
		compression_level = (size_t)info[1].ToNumber().Int32Value();
	}
	else
	{
		std::string error_message = "compress(uint8array) or compress(uint8array, compression level)";
		throw TypeError::New(info.Env(), error_message);
	}

	uint8_t *input_data = to_compress.Data();
	size_t total = to_compress.ElementLength();

	std::vector<uint8_t> data(to_compress.ElementLength());

	copy_buffer_data(input_data, data.data(), total);

	CompressWorker* worker = new CompressWorker(
		info.Env(),
		data,
		compression_level
	);

	worker -> Queue();

	return worker->GetPromise();
}

Napi::Promise Decompress(const CallbackInfo &info)
{
	auto number_of_args = info.Length();
	Napi::Uint8Array compressed_data;

	if (number_of_args == 0)
	{
		std::string error_message = "decompress(uint8array)";
		throw TypeError::New(info.Env(), error_message);
	}
	else if (number_of_args == 1)
	{
		compressed_data = Uint8ArrayFromValue(info[0], "buffer");
	}
	else
	{
		std::string error_message = "decompress(uint8array)";
		throw TypeError::New(info.Env(), error_message);
	}

	uint8_t *input_data = compressed_data.Data();
	size_t total = compressed_data.ElementLength();

	byte_vec data(total);
	copy_buffer_data(input_data, data.data(), total);


	DecompressWorker* worker = new DecompressWorker(
		info.Env(),
		data,
		total * 100
	);

	worker -> Queue();

	return worker->GetPromise();
}

Value getFrameHeader(const CallbackInfo &info)
{
	if (info.Length() != 1)
	{
		throw TypeError::New(info.Env(), "must provide a buffer.");
	}

	Napi::Value arg_buffer = info[0];
	auto parse_arg = [](Napi::Value v) -> Uint8Array
	{
		if (!v.IsTypedArray() || v.As<TypedArray>().TypedArrayType() != napi_uint8_array)
		{
			std::string error_message = "Parameter must be a Uint8Array.";
			throw TypeError::New(v.Env(), error_message);
		}
		return v.As<Uint8Array>();
	};

	Uint8Array buffer = parse_arg(arg_buffer);
	FrameHeader header(buffer);

	Object return_value = Object::New(info.Env());

	return_value["windowSize"] = BigInt::New(info.Env(), uint64_t(header.windowSize));
	return_value["frameContentSize"] = header.frameContentSize == ZSTD_CONTENTSIZE_UNKNOWN ? String::New(info.Env(), "unknown") : BigInt::New(info.Env(), uint64_t(header.frameContentSize));
	return_value["headerSize"] = BigInt::New(info.Env(), uint64_t(header.headerSize));
	return_value["dictID"] = BigInt::New(info.Env(), uint64_t(header.dictID));
	return_value["checksumFlag"] = BigInt::New(info.Env(), uint64_t(header.checksumFlag));
	return_value["_reserved1"] = BigInt::New(info.Env(), uint64_t(header._reserved1));
	return_value["_reserved2"] = BigInt::New(info.Env(), uint64_t(header._reserved2));
	return_value["frameType"] = String::New(info.Env(),
											header.frameType == ZSTD_frame ? "zstd frame" : "zstd skippable frame");

	return return_value;
}

Napi::Object Init(Napi::Env env, Napi::Object exports)
{
	exports["parseFrameHeader"] = Napi::Function::New(env, getFrameHeader);
	exports.Set(Napi::String::New(env, "compress"),
				Napi::Function::New(env, Compress));
	exports.Set(Napi::String::New(env, "decompress"),
				Napi::Function::New(env, Decompress));
	return exports;
}

NODE_API_MODULE(hello, Init)