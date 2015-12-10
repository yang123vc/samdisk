// FDC-like flux reversal decoding
//
// PLL code from Keir Frasier's Disk-Utilities/libdisk

#include "SAMdisk.h"
#include "FluxDecoder.h"

FluxDecoder::FluxDecoder (const std::vector<std::vector<uint32_t>> &flux_revs, int bitcell_ns, int flux_scale_percent)
	: m_flux_revs(flux_revs), m_clock(bitcell_ns), m_clock_centre(bitcell_ns),
	m_clock_min(bitcell_ns * (100 - CLOCK_MAX_ADJUST) / 100),
	m_clock_max(bitcell_ns * (100 + CLOCK_MAX_ADJUST) / 100),
	m_flux_scale_percent(flux_scale_percent)
{
	assert(flux_revs.size());
	assert(flux_revs[0].size());

	m_rev_it = m_flux_revs.cbegin();
	m_flux_it = (*m_rev_it).cbegin();
}

size_t FluxDecoder::bitstream_size () const
{
	size_t total_size = 0;

	for (const auto &vec : m_flux_revs)
		total_size += vec.size();

	return total_size * 8;
}

bool FluxDecoder::index ()
{
	auto ret = m_index;
	m_index = false;
	return ret;
}

int FluxDecoder::next_bit ()
{
	int new_flux;

	while (m_flux < m_clock / 2)
	{
		if ((new_flux = next_flux()) == -1)
			return -1;

		if (m_flux_scale_percent != 100)
			new_flux = new_flux * m_flux_scale_percent / 100;

		m_flux += new_flux;
		m_clocked_zeros = 0;
	}

	m_flux -= m_clock;

	if (m_flux >= m_clock / 2)
	{
		++m_clocked_zeros;
		return 0;
	}

	// PLL: Adjust clock frequency according to phase mismatch
	if ((m_clocked_zeros >= 1) && (m_clocked_zeros <= 3))
	{
		// In sync: adjust base clock by percentage of phase mismatch
		auto diff = m_flux / (m_clocked_zeros + 1);
		m_clock += diff / CLOCK_MAX_ADJUST;
	}
	else
	{
		// Out of sync: adjust base clock towards centre
		m_clock += (m_clock_centre - m_clock) / CLOCK_MAX_ADJUST;
	}

	// Clamp the clock's adjustment range
	m_clock = std::min(std::max(m_clock_min, m_clock), m_clock_max);

	// Authentic PLL: Do not snap the timing window to each flux transition
	new_flux = m_flux / 2;
	m_flux = new_flux;

	return 1;
}

int FluxDecoder::next_flux ()
{
	if (m_flux_it == (*m_rev_it).cend())
	{
		if (++m_rev_it == m_flux_revs.cend())
			return -1;

		m_index = true;
		m_flux_it = (*m_rev_it).cbegin();
	}

	auto time_ns = *m_flux_it++;
	return time_ns;
}