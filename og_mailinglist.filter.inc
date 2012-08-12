<?php
/**
 * @file
 * OG Mailinglist filter to remove or hide quotes in emails.
 **/

/**
 * Implements hook_filter_info().
 */
function og_mailinglist_filter_info() {
  $filters['filter_email_quote_hider'] = array(
    'title' => t('Hide quotations in emails'),
    'description' => t('Finds quoted text from comments created via email and adds Show / Hide link.'),
    'process callback' => '_og_mailinglist_build_quotes_toggle',
    'tips callback' => '_og_mailinglist_filter_tips',
    'settings callback' => '_og_mailinglist_filter_settings',
    'default settings' => array(
      'og_mailinglist_filter_quotations' => 'hidden',
      'og_mailinglist_filter_email' => 1,
    ),
  );
  return $filters;
}

function _og_mailinglist_filter_settings($form, &$form_state, $filter, $format, $defaults) {
  $filter->settings += $defaults;
  
  $settings['og_mailinglist_filter_quotations'] = array(
    '#title' => t('Quotations style'),
    '#type' => 'radios',
    '#options' => array(
      'hidden' => t('Quotations will be hidden with a JavaScript "Show" link. (default)'),
      'removed' => t('Quotations will be removed altogether.'),
    ),
    '#default_value' => $filter->settings['og_mailinglist_filter_quotations'],
    '#description' => t('E-mail style quotations are lines starting with ">" marks.'),
  );
  $settings['og_mailinglist_filter_email'] = array(
    '#title' => t('Hide email addresses.'),
    '#type' => 'checkbox',
    '#default_value' => $filter->settings['og_mailinglist_filter_email'],
    '#description' => t('If checked, email addresses will be replaced with ***@***.*** (enabled by default)'),
  );
  return $settings;
}

function _og_mailinglist_filter_tips($filter, $format, $long = FALSE) {
  if ($long){
    $output = t('Email quotation hider hides all quotations by default and provides a link to show quoted text.');
    $output .= '<br />';
    $output .= t('It also replaces all e-mail addresses with ***@***.***');
    return $output;
  }
  else {
    return t('Email quotation hider makes quotations in emails hidden by default and replaces all email addresses with ***@***.***');
  }
}

/**
 * Surround quotes within an email text to make the text hidden by default.
 *
 * @param string $text
 *  Text where to hide quotes.
 *
 * @param $filter
 *  Filter passes on e.g settings.
 *
 * @return
 *  Text with hidden quotes, contains HTML.
 */
function _og_mailinglist_build_quotes_toggle($text, $filter) {
  module_load_include('inc', 'og_mailinglist', 'og_mailinglist_filter');

  if ($filter->settings['og_mailinglist_filter_quotations'] == 'removed') {
    # Remove all quoted text instead.
    return _og_mailinglist_remove_quotes($text);
  }
  $text = str_replace(array('\r\n', '\r'), '\n', $text);
  $quotes = _og_mailinglist_find_quoted_sections($text);

  if (!empty($quotes)) {
    foreach ($quotes as $quote) {
      // Run the quote through the Line break filter before replacing it in the text.
      // Otherwise the HTML Converter input filter will bork the HTML. This *could* be a
      // potential problem if someone doesn't want the Line Break filter run.
      $filtered_quote = _filter_autop($quote);
      $replace = '<br /><span class="toggle-quoted-text">- '
        . t('Show quoted text')
        . ' -</span><div class="quoted-text">'
        . $filtered_quote . '</div>';
      $text = str_replace($quote, $replace, $text);
      
      if ($filter->settings['og_mailinglist_filter_email'] == 1) {
        // Remove email addresses. Regex borrowed from valid_email_address().
        $regex = "([a-zA-Z0-9_\-\.\+\^!#\$%&*+\/\=\?\`\|\{\}~\']+@(?:(?:[a-zA-Z0-9]|[a-zA-Z0-9][a-zA-Z0-9\-]*[a-zA-Z0-9])\.?)+)";
        $text = preg_replace($regex, '***@***.***', $text);
      }
    }
  }

  return $text;
}

/**
 * Remove quotes from e-mail text entirely.
 *
 * @param string $text
 *  Text to remove quotes from.
 *
 * @return
 *  Text with quotes removed.
 */
function _og_mailinglist_remove_quotes($text) {
  module_load_include('inc', 'og_mailinglist', 'og_mailinglist_filter');
  $text = str_replace(array('\r\n', '\r'), '\n', $text);
  $quotes = _og_mailinglist_find_quoted_sections($text);
  if (!empty($quotes)) {
    foreach ($quotes as $quote) {
      $text = str_replace($quote, '', $text);
    }
  }
  return $text;
}

// Possible way to simplify this. Drop the find start of quoted section and loop through it
// and just do as doing with Hotmail/Outlook and other clients that don't quote
// properly and match all the quoted section w/ regex.
// This rests on the assumption that everyone will either top-post or
// properly trim their emails where they bottom post. It seems the vast majority
// of people do that.
// Making the switch would vastly simplify the code.
// We could then hold the regex for each email client in an array and just pass
// that and the text into a function.
function _og_mailinglist_find_quoted_sections($text) {
  $quotes = array();

  // Some email clients are "special". They don't like to place ">" in front of
  // quotes. We need to give them their own special functions.
  $quotes[] = array();
  $quotes[] = _og_mailinglist_filter_hotmail_quotes($text);
  $quotes[] = _og_mailinglist_filter_outlook_quotes($text);
  $quotes[] = _og_mailinglist_filter_android_gmail_quotes($text);
  $quotes[] = _og_mailinglist_filter_unknown_client_quotes($text);
  $quotes[] = _og_mailinglist_filter_unknown_client2_quotes($text);
  $quotes[] = _og_mailinglist_filter_unknown_client3_quotes($text);
  $quotes[] = _og_mailinglist_filter_unknown_client3a_quotes($text);
  $quotes[] = _og_mailinglist_filter_unknown_client4_quotes($text);
  $quotes[] = _og_mailinglist_filter_unknown_client4a_quotes($text);
  $quotes[] = _og_mailinglist_filter_unknown_client4b_quotes($text);
  $quotes[] = _og_mailinglist_filter_unknown_client5_quotes($text);
  $quotes[] = _og_mailinglist_filter_quotes($text);

  // Identify the longest quoted section of all possible matches and only
  // remove that one.
  $quotes = _og_mailinglist_longest_string($quotes);

  return $quotes;
}

/*
 * Identify the longest quoted section as that should contain all the others
 * that are possibly within it.
 */
function _og_mailinglist_longest_string($quotes) {
  $longest = array();
  foreach ($quotes as $quote) {
    if (key($longest) < strlen(implode("", $quote))) {
      $longest = array(strlen(implode("", $quote)) => $quote);
    }
  }

  return end($longest);
}

/*
 * Loops through the text line by line. If a line matches the header of
 * an email client's quoted section, that line is marked as a beginning of a
 * quoted section. Each succeeding line which starts with a ">"
 * (the traditional indicator for email quote) is included in the quoted section.
 **/
function _og_mailinglist_filter_quotes($text) {
  $lines = explode("\n", $text);
  $quotes = array();
  $count = 0;
  $in_quote = FALSE;
  foreach ($lines as $line) {
    if (preg_match("/On\s\w{3},\s\w{3}\s\d{1,2},\s\d{4}\sat\s\d{1,2}:\d{2}\s[PM|AM]/s", #Gmail
                             $line, $matches)
       || preg_match("/On\s\w{3}\s\d{1,2},\s\d{4},\sat\s\d{1,2}:\d{2}\s(AM|PM),\s/s", #Apple mail
                            $line, $matches)
       || preg_match("/On\s\w{3}\s\d{2},\s\d{4},\sat\s[0-9:]{4,5}\s(PM|AM),\s[a-zA-Z\s]+wrote:/s", #unknown as of yet
                            $line, $matches)
       || preg_match("/On\s\d{1,2}\/\d{1,2}\/\d{1,2},\s.+?<[a-zA-Z0-9_\-\.\+\^!#\$%&*+\/\=\?\`\|\{\}~\']+@(?:(?:[a-zA-Z0-9]|[a-zA-Z0-9][a-zA-Z0-9\-]*[a-zA-Z0-9])\.?)+>\swrote:/s", # Some mobile email client, blackberry maybe?
                            $line, $matches)
       || preg_match("/On\s\w{3,10},\s\w{3,9}\s\d{1,2},\s\d{4},\s.+?wrote:/s", #unknown as of yet "On Saturday, May 22, 2010, Joe Smith wrote:"
                            $line, $matches)
       || preg_match("/On.\d{1,2}\/\d{1,2}\/\d{1,2}.\d{1,2}:\d{1,2}\s(PM|AM),.+?wrote:/s", #unknown as of yet "On 5/21/10 8:43 PM, Joe Smith wrote:"
                            $line, $matches)
       || preg_match("/On.\d{1,2}\/\d{1,2}\/\d{1,2}.\d{1,2}:\d{1,2},.+?wrote:/s", #unknown as of yet "On 30/01/11 22:38, Joe Smith wrote:"
                            $line, $matches)
       || preg_match("/On\s\d{1,2}\s\w{3,9}\s\d{4}\s\d{1,2}:\d{1,2},?\s.+?wrote:/s", #unknown as of yet "On 1 February 2011 10:24, Joe Smith wrote:" (the comma after the time is optional)
                            $line, $matches)
       || preg_match("/On\s\d{1,2}\s\w{3,9}\s\d{4},\sat\s\d{1,2}:\d{1,2},\s.+?wrote:/s", #unknown as of yet "On 30 Jan 2011, at 13:18, Joe Smith  wrote:"
                            $line, $matches)
       || preg_match("/On.\d{1,2}\/\d{1,2}\/\d{1,2}.\d{1,2}:\d{1,2},.+?wrote:/s", #Thunderbird 24hr "On 21/5/10 13:45, Joe Smith wrote:"
                            $line, $matches)
       && !$in_quote) {


      $quotes[$count][] = $line;
      $in_quote = TRUE;
    }
    else if ($in_quote && (trim($line) === "" || substr(trim($line), 0, 1) === ">"
      // or the previous line ended w/ a "<" Long emails addresses
      // (e.g. <someaddress@gmail.com>) sometimes get truncated
      // leaving the second part of the line on the next line.
            || substr(trim(array_pop(end($quotes))), -1) === "<"
      // Same situation as above. But here the <someaddress@gmail.com> is
      // brought down to the next line so the < is at the start of the line.
            || substr(trim($line), 0, 1) === "<"
      // And wrote: is also sometimes shoved down to the next line.
            || substr(trim($line), 0, 6) === "wrote:")) {
      $quotes[$count][] = $line;
    }
    else {
      $in_quote = false;
      $count++;
    }
  }
  $quotes_imploded = array();
  foreach ($quotes as $quote) {
    $quotes_imploded[] = implode($quote, "\n");
  }
  return $quotes_imploded;
}

/**
 * Filter quotes out of emails from hotmail.com.
 *
 * The assumption here is that once the headers to the quoted section begins,
 * all content after that is quotes. This seems to be how in practice people
 * send emails but occasionaly this might filter the wrong text.
 */
function _og_mailinglist_filter_hotmail_quotes($text) {
  $regex = "/Date:\s\w{3},\s\d{1,2}\s\w{3}\s\d{4}\s\d{2}:\d{2}:\d{2}\s-\d{4}.Subject:\s.*?From:.*?To:\s.*/s";
  preg_match($regex, $text, $matches);
  if (!empty($matches)) {
    return $matches;
  }
  else {
    return array();
  }
}

/**
 * Filter quotes out of emails from hotmail.com.
 */
function _og_mailinglist_filter_outlook_quotes($text) {
  $regex = "/From:\s\w+\s\w+.+?\[mailto:\w+.+?Sent:\s\w+,\s\w+\s\d{2},\s\d{4}\s\d{1,2}:\d{1,2}\s[AM|PM]+.To:\s.+?Subject:\s.*/s";
  preg_match($regex, $text, $matches);
  if (!empty($matches)) {
    return $matches;
  }
  else {
    return array();
  }
}

/**
 * Filter quotes out of emails from Android's GMail client.
 */
function _og_mailinglist_filter_android_gmail_quotes($text) {
  $regex = "/On\s\w{3}\s\d{1,2},\s\d{4}\s\d{1,2}:\d{2}\s[PM|AM]+,\s.*/s";
  preg_match($regex, $text, $matches);
  if (!empty($matches)) {
    return $matches;
  }
  else {
    return array();
  }
}

/**
 * Filter quotes out of emails from some other microsoftie looking email client.
 */
function _og_mailinglist_filter_unknown_client_quotes($text) {
  $regex = "/From:\s\".*?\"\s<.*?@.*?>.Date:\s\w{3},.*?To:.*?<.*?@.*?>.*?Cc:\s.*?<.*?@.*?>.*?Subject:.*/s";
  preg_match($regex, $text, $matches);
  if (!empty($matches)) {
    return $matches;
  }
  else {
    return array();
  }
}

/**
 * Filter quotes out of emails from some other microsoftie looking email client.
 */
function _og_mailinglist_filter_unknown_client2_quotes($text) {
  $regex = "/From:\s.*?<.*?@.*?>.To:.*?@.*?Sent:\s\w{3},\s.*?\d{1,2},\s\d{4}\s.*?Subject:\s.*/s";
  preg_match($regex, $text, $matches);
  if (!empty($matches)) {
    return $matches;
  }
  else {
    return array();
  }
}

/**
 * Filter quotes out of emails from yet another microsoftie looking email client.
 *
 * From: Dave [mailto:bob@example.com]
 * Sent: Monday, April 05, 2010 5:30 PM
 * To: A group
 * Subject: [a-group] SQL Server Express
 *
 */
function _og_mailinglist_filter_unknown_client3_quotes($text) {
  $regex = "/From:\s.*?\[.*?@.*?\].*?Sent:\s\w*?,\s\w*?\s\d{1,2},\s\d{4}\s\d{1,2}:\d{2}\s(PM|AM).*?To:\s.*?Subject:.*/s";
  preg_match($regex, $text, $matches);
  if (!empty($matches)) {
    return $matches;
  }
  else {
    return array();
  }
}

/**
 * Filter quotes out of emails from yet another microsoftie looking email client.
 *
 * From: Someone [mailto:someone@gmail.com]
 * Sent: Thursday, 23 September 2010 9:55 AM // Diff from above is in date format.
 * To: test-two@lists.somewhere.com
 * Subject: [test-two] Thursday morning: Forum start via email
 *
 */
function _og_mailinglist_filter_unknown_client3a_quotes($text) {
  $regex = "/From:\s.*?\[.*?@.*?\].*?Sent:\s\w*?,\s\d{1,2}\s\w*?\s\d{4}\s\d{1,2}:\d{2}\s(PM|AM).*?To:\s.*?Subject:.*/s";
  preg_match($regex, $text, $matches);
  if (!empty($matches)) {
    return $matches;
  }
  else {
    return array();
  }
}

/**
 * Filter quotes out of emails from yet another microsoftie looking email client.
 *
 * -----Original Message-----
 * From: Someone <someone@me.com>
 * Date: Sat, 10 Apr 2110 12:25:28
 * To: Entrepreneurship<entrepreneurship@island.byu.edu>
 * Subject: Re: [entrepreneurship] Need Tech Guys to Form
 *
 */
function _og_mailinglist_filter_unknown_client4_quotes($text) {
  $regex = "/-----Original\sMessage-----.*?From:\s.*?@.*?Date:\s\w{3},\s\d{1,2}\s.*?:.*?:.*?To:.*?@.*?Subject:.*/s";
  preg_match($regex, $text, $matches);
  if (!empty($matches)) {
    return $matches;
  }
  else {
    return array();
  }
}

/**
 * Filter quotes out of emails from yet another microsoftie looking email client.
 *
 * -----Original Message-----
 * From: someone [mailto:someone@somewhere.com]
 * Sent: Friday, 24 September 2010 10:54 AM
 * To: test-one@lists.somewhere.com
 * Subject: Re: [test-one] Test to Group 1 From Email
 *
 */
function _og_mailinglist_filter_unknown_client4a_quotes($text) {
  $regex = "/-----Original\sMessage-----.*?From:\s.*?@.*?Sent:\s\w{3,9},\s\d{1,2}\s.*?:.*?:.*?To:.*?@.*?Subject:.*/s";
  preg_match($regex, $text, $matches);
  if (!empty($matches)) {
    return $matches;
  }
  else {
    return array();
  }
}

/**
 * Filter quotes out of emails from yet another microsoftie looking email client.
 *
 * ----- Original Message -----
 * From: "John someone" <someone@gmail.com>
 * To: test-one@lists.somewhere.com
 * Subject: [test-one] Test to Group 1 From Email
 * Date: Fri, 24 Sep 2010 09:36:57 +1000
 *
 */
function _og_mailinglist_filter_unknown_client4b_quotes($text) {
  $regex = "/-----\sOriginal\sMessage\s-----.*?From:\s.*?\<.*?@.*?\>.*?To:\s.*?Subject:\s.*?Date:\s\w*?,\s\d{1,2}\s\w*?\s\d{4}\s\d{1,2}:\d{2}:\d{2}\s.*/s";
  preg_match($regex, $text, $matches);
  if (!empty($matches)) {
    return $matches;
  }
  else {
    return array();
  }
}

/**
 * Filter quotes out of emails from yet another email client.
 *
 * On 15 April 2010 21:57, someone someone@example.com wrote:
 *
 */
function _og_mailinglist_filter_unknown_client5_quotes($text) {
  $regex = "/On\s\d{1,2}\s\w.*?\s\d{4}\s\d{2}:\d{2},\s.*?@.*?\s.*?wrote:.*/s";
  preg_match($regex, $text, $matches);
  if (!empty($matches)) {
    return $matches;
  }
  else {
    return array();
  }
}
