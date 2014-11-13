#include <sndfile.hh>
#include <soxr.h>
#include <speex/speex_resampler.h>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <stdexcept>
#include <memory>
#include <utility>

//! Audio frame duration, in ms
const unsigned int frame_duration = 20;

struct resampler_base
{
	virtual ~resampler_base() {}
	virtual void process(const void* in_samples, std::size_t in_size, std::size_t& in_consumed, void* out_samples, std::size_t out_size, std::size_t& out_produced) = 0;
};

struct speex_resampler_base :
	public resampler_base
{
	SpeexResamplerState* m_state;

	speex_resampler_base(int in_rate, int in_channels, int out_rate, int quality) : m_state(speex_resampler_init(in_channels, in_rate, out_rate, quality, NULL))
	{
		if (!m_state)
			throw std::runtime_error("Failed to create speex resampler context");

		speex_resampler_skip_zeros(m_state);
	}

	~speex_resampler_base()
	{
		speex_resampler_destroy(m_state);
	}
};

struct speex_int_resampler :
	public speex_resampler_base
{
	speex_int_resampler(int in_rate, int in_channels, int out_rate, int quality) : speex_resampler_base(in_rate, in_channels, out_rate, quality)
	{
	}

	void process(const void* in_samples, std::size_t in_size, std::size_t& in_consumed, void* out_samples, std::size_t out_size, std::size_t& out_produced)
	{
		spx_uint32_t in_len = in_size, out_len = out_size;
		speex_resampler_process_interleaved_int(m_state, static_cast< const spx_int16_t* >(in_samples), &in_len, static_cast< spx_int16_t* >(out_samples), &out_len);
		in_consumed = in_len;
		out_produced = out_len;
	}
};

struct speex_float_resampler :
	public speex_resampler_base
{
	speex_float_resampler(int in_rate, int in_channels, int out_rate, int quality) : speex_resampler_base(in_rate, in_channels, out_rate, quality)
	{
	}

	void process(const void* in_samples, std::size_t in_size, std::size_t& in_consumed, void* out_samples, std::size_t out_size, std::size_t& out_produced)
	{
		spx_uint32_t in_len = in_size, out_len = out_size;
		speex_resampler_process_interleaved_float(m_state, static_cast< const float* >(in_samples), &in_len, static_cast< float* >(out_samples), &out_len);
		in_consumed = in_len;
		out_produced = out_len;
	}
};

struct soxr_resampler :
	public resampler_base
{
	soxr_t m_state;

	soxr_resampler(int in_rate, int in_channels, int in_format, int out_rate, int recipe)
	{
		soxr_io_spec_t io_spec;
		switch (in_format)
		{
		case SF_FORMAT_PCM_16:
			io_spec = soxr_io_spec(SOXR_INT16_I, SOXR_INT16_I);
			break;
		case SF_FORMAT_PCM_32:
			io_spec = soxr_io_spec(SOXR_INT32_I, SOXR_INT32_I);
			break;
		case SF_FORMAT_FLOAT:
			io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
			break;
		default:
			throw std::invalid_argument("Unsupported sample format for soxr");
		}

		soxr_quality_spec_t quality = soxr_quality_spec(recipe, 0);
		m_state	= soxr_create(in_rate, out_rate, in_channels, NULL, &io_spec, &quality, NULL);

		if (!m_state)
			throw std::runtime_error("Failed to create soxr context");
	}

	~soxr_resampler()
	{
		soxr_delete(m_state);
	}

	void process(const void* in_samples, std::size_t in_size, std::size_t& in_consumed, void* out_samples, std::size_t out_size, std::size_t& out_produced)
	{
		soxr_process(m_state, in_samples, in_size, &in_consumed, out_samples, out_size, &out_produced);
	}
};

inline std::unique_ptr< resampler_base > create_resampler(std::string const& type, int in_rate, int in_channels, int in_format, int out_rate)
{
	std::unique_ptr< resampler_base > p;

	if (type.substr(0, 6) == "speex-")
	{
		std::istringstream strm(type.substr(6));
		int quality = 0;
		strm >> quality;

		switch (in_format)
		{
		case SF_FORMAT_PCM_16:
			p.reset(new speex_int_resampler(in_rate, in_channels, out_rate, quality));
			break;
		case SF_FORMAT_FLOAT:
			p.reset(new speex_float_resampler(in_rate, in_channels, out_rate, quality));
			break;
		default:
			throw std::invalid_argument("Unsupported sample format for speex resampler");
		}
	}
	else if (type.substr(0, 5) == "soxr-")
	{
		int recipe = SOXR_QQ;
		if (type == "soxr-lq")
			recipe = SOXR_LQ;
		else if (type == "soxr-mq")
			recipe = SOXR_MQ;
		else if (type == "soxr-hq")
			recipe = SOXR_HQ;
		else if (type == "soxr-vhq")
			recipe = SOXR_VHQ;

		p.reset(new soxr_resampler(in_rate, in_channels, in_format, out_rate, recipe));
	}
	else
	{
		throw std::invalid_argument("Unrecognized resampler: " + type);
	}

	return std::move(p);
}

template<
	typename SampleType,
	sf_count_t (SndfileHandle::*ReadF)(SampleType*, sf_count_t),
	sf_count_t (SndfileHandle::*WriteF)(const SampleType*, sf_count_t)
>
inline void resampling_loop(SndfileHandle& in_file, SndfileHandle& out_file, resampler_base& resampler)
{
	struct local
	{
		static void write_data(SndfileHandle& in_file, const SampleType* out_data, std::size_t out_size, std::size_t total_consumed, std::size_t& total_produced, SndfileHandle& out_file)
		{
			if (total_produced == 0)
			{
				// This is the first produced output, calculate the resampler delay
				float delay = total_consumed - (static_cast< float >(out_size) * in_file.samplerate() / out_file.samplerate());
				std::cout << "Resampler delay: " << std::fixed << std::setprecision(3) << delay << " samples (" << static_cast< float >(delay) * 1000.0f / in_file.samplerate() << " ms)" << std::endl;
			}

			total_produced += out_size;
			sf_count_t written_count = (out_file.*WriteF)(out_data, out_size);
			if (written_count != out_size)
				throw std::runtime_error("Failed to write samples to the output file");
		}
	};

	std::size_t in_frame_size = (in_file.samplerate() * frame_duration + 999u) / 1000u;
	std::size_t out_frame_size = (out_file.samplerate() * frame_duration + 999u) / 1000u;

	const std::size_t channel_count = in_file.channels();
	std::unique_ptr< SampleType[] > in_data(new SampleType[in_frame_size * channel_count]);
	std::unique_ptr< SampleType[] > out_data(new SampleType[out_frame_size * channel_count]);
	std::size_t in_size = 0, total_consumed = 0, total_produced = 0;

	std::size_t in_count = in_file.frames();
	while (total_consumed < in_count)
	{
		const std::size_t packs_to_read = std::min(in_frame_size - in_size, in_count - total_consumed);
		sf_count_t read_count = (in_file.*ReadF)(in_data.get() + in_size, packs_to_read);
		if (read_count != packs_to_read)
			throw std::runtime_error("Failed to read samples from the input file");
		in_size += read_count;

		std::size_t consumed = 0, produced = 0;
		resampler.process(in_data.get(), in_size, consumed, out_data.get(), out_frame_size, produced);

		total_consumed += consumed;
		in_size -= consumed;
		std::memmove(in_data.get(), in_data.get() + consumed * channel_count, in_size * channel_count * sizeof(SampleType));

		if (produced > 0)
			local::write_data(in_file, out_data.get(), produced, total_consumed, total_produced, out_file);
	}

	// Flush the resampler
	while (true)
	{
		std::size_t consumed = 0, produced = 0;
		resampler.process(NULL, 0, consumed, out_data.get(), out_frame_size, produced);

		if (produced > 0)
			local::write_data(in_file, out_data.get(), produced, total_consumed, total_produced, out_file);
		else
			break;
	}
}

int main(int argc, char* argv[])
{
	try
	{
		if (argc < 5)
			throw std::invalid_argument("Usage: src_test <input> <resampler> <output rate> <output>");

		std::string in_filename = argv[1];
		std::string out_filename = argv[4];
		int out_rate = 0;
		{
			std::istringstream strm(argv[3]);
			strm >> out_rate;
			if (out_rate <= 0)
				throw std::invalid_argument("Invalid sample rate");
		}

		SndfileHandle in_file(in_filename);
		if (!in_file)
			throw std::runtime_error("Failed to open input file " + in_filename);

		std::cout << in_filename << " opened:\nSample rate: " << in_file.samplerate() << "\nSample format: 0x" << std::hex << in_file.format() << std::dec << "\nChannels: " << in_file.channels() << std::endl;

		if (in_file.channels() != 1 && in_file.channels() != 2)
			throw std::invalid_argument("Unsupported input channel count");

		unsigned int sample_format = in_file.format() & SF_FORMAT_SUBMASK;
		SndfileHandle out_file(out_filename, SFM_WRITE, sample_format | SF_FORMAT_WAV, in_file.channels(), out_rate);
		if (!out_file)
			throw std::runtime_error("Failed to open output file " + out_filename);

		std::cout << out_filename << " opened:\nSample rate: " << out_file.samplerate() << "\nSample format: 0x" << std::hex << out_file.format() << std::dec << "\nChannels: " << out_file.channels() << std::endl;

		std::unique_ptr< resampler_base > resampler = create_resampler(argv[2], in_file.samplerate(), in_file.channels(), sample_format, out_rate);

		switch (sample_format)
		{
		case SF_FORMAT_PCM_16:
			resampling_loop< short, (sf_count_t (SndfileHandle::*)(short*, sf_count_t))&SndfileHandle::readf, (sf_count_t (SndfileHandle::*)(const short*, sf_count_t))&SndfileHandle::writef >(in_file, out_file, *resampler);
			break;
		case SF_FORMAT_FLOAT:
			resampling_loop< float, (sf_count_t (SndfileHandle::*)(float*, sf_count_t))&SndfileHandle::readf, (sf_count_t (SndfileHandle::*)(const float*, sf_count_t))&SndfileHandle::writef >(in_file, out_file, *resampler);
			break;
		default:
			throw std::invalid_argument("Unsupported sample format");
		}
	}
	catch (std::exception& e)
	{
		std::cout << "FAILURE: " << e.what() << std::endl;
		return 1;
	}

	return 0;
}

